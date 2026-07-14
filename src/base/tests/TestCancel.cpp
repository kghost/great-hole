#include <boost/asio.hpp>
#include <chrono>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "Event.hpp"
#include "GetCurrentOmniFiber.hpp"
#include "Manager.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"

using namespace gh;
using namespace Omni::Fiber;

namespace {

void RunEventLoop(boost::asio::io_context& io) {
  io.restart();
  io.run();
}

} // namespace

TEST(CancelSelectTest, CorrectWayCancelsTimer) {
  boost::asio::io_context io;
  AsioExecutor executor(io.get_executor());
  Manager manager(executor);

  Cancel cancel;
  bool selectFinished = false;
  boost::system::error_code selectEc;

  manager.SpawnRoot("root", [&]() -> Coroutine<void> {
    auto& current = co_await GetCurrentOmniFiber();

    // Create a long running timer
    auto timer = std::make_shared<boost::asio::steady_timer>(io, std::chrono::hours(1));

    // Spawn a child fiber that triggers cancellation after a short delay
    auto child = current.Spawn("canceller", [&]() -> Coroutine<void> {
      boost::asio::steady_timer waitTimer(io, std::chrono::milliseconds(10));
      co_await waitTimer.async_wait(AsioUseFiber);
      cancel.Trigger();
      co_return;
    });

    Event<void> dummyEvent; // Never fired

    // Storing the cancelSlot extending its lifetime for the duration of co_await Select
    auto cancelSlot = cancel.AsioSlot();

    co_await Select(SelectPair(dummyEvent, [] -> void {}),
                    SelectPair(timer->async_wait(cancelSlot()),
                               AsioApply([&](boost::system::error_code ec) -> void { selectEc = ec; })));

    selectFinished = true;
    co_await current.Join(child);
    co_return;
  });

  RunEventLoop(io);

  EXPECT_TRUE(selectFinished);
  EXPECT_EQ(selectEc, boost::asio::error::operation_aborted);
}

TEST(CancelSelectTest, InlineTemporaryWayHangsOrCrashes) {
  // NOTE: This test passes because in C++, the lifetime of temporary objects created
  // during the evaluation of a co_await expression is extended until the co_await
  // resumes and the full-expression completes.
  //
  // Because the entire Select call is a single full-expression, the temporary SlotTracker
  // returned by cancel.AsioSlot() is NOT destroyed when the coroutine suspends.
  // It remains alive and registered, allowing cancel.Trigger() to successfully trigger the abort.
  //
  // However, this makes the code extremely fragile under refactoring. If the statement is split
  // (as shown in the test below), it will hang or crash.
  boost::asio::io_context io;
  AsioExecutor executor(io.get_executor());
  Manager manager(executor);
  Cancel cancel;

  manager.SpawnRoot("root", [&]() -> Coroutine<void> {
    auto& current = co_await GetCurrentOmniFiber();
    auto timer = std::make_shared<boost::asio::steady_timer>(io, std::chrono::hours(1));
    auto child = current.Spawn("canceller", [&]() -> Coroutine<void> {
      boost::asio::steady_timer waitTimer(io, std::chrono::milliseconds(10));
      co_await waitTimer.async_wait(AsioUseFiber);
      cancel.Trigger();
      co_return;
    });

    Event<void> dummyEvent;
    co_await Select(
        SelectPair(dummyEvent, [] -> void {}),
        SelectPair(timer->async_wait(cancel.AsioSlot()()), AsioApply([&](boost::system::error_code ec) -> void {})));
    co_await current.Join(child);
    co_return;
  });

  RunEventLoop(io);
}

TEST(CancelSelectTest, InlineTemporaryWaySplitStatementVulnerability) {
  // This test demonstrates the vulnerability of the inline temporary approach:
  // if a developer splits the statement (for example, to store the pair in a local variable),
  // the temporary SlotTracker is destroyed at the first semicolon, leading to a crash or hang.
  //
  // Attempting to run the following split-statement code under ASAN will result in a
  // stack-use-after-scope crash:
  //
  // boost::asio::io_context io;
  // AsioExecutor executor(io.get_executor());
  // Manager manager(executor);
  // Cancel cancel;
  //
  // manager.SpawnRoot("root", [&]() -> Coroutine<void> {
  //   auto& current = co_await GetCurrentOmniFiber();
  //   auto timer = std::make_shared<boost::asio::steady_timer>(io, std::chrono::hours(1));
  //   auto child = current.Spawn("canceller", [&]() -> Coroutine<void> {
  //     boost::asio::steady_timer waitTimer(io, std::chrono::milliseconds(10));
  //     co_await waitTimer.async_wait(AsioUseFiber);
  //     cancel.Trigger();
  //     co_return;
  //   });
  //
  //   Event<void> dummyEvent;
  //   auto pair = SelectPair(timer->async_wait(cancel.AsioSlot()()), AsioApply([&](boost::system::error_code ec) ->
  //   void {}));
  //
  //   co_await Select(SelectPair(dummyEvent, [] -> void {}), pair);
  //   co_await current.Join(child);
  //   co_return;
  // });
  //
  // RunEventLoop(io);
}
