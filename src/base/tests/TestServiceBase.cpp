#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "Manager.hpp"
#include "ServiceBase.hpp"

using namespace gh;

namespace {

class MockService final : public ServiceBase {
public:
  MockService() = default;
  ~MockService() override = default;

  auto GetName() const -> std::string override { return "MockService"; }

protected:
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override { co_return ErrorCode{}; }

  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override { co_return ErrorCode{}; }
};

void RunEventLoop(boost::asio::io_context& io) {
  io.restart();
  io.run();
}

} // namespace

TEST(ServiceBaseTest, StopReentrantAfterWaitService) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto service = std::make_shared<MockService>();
    auto errStart = co_await service->Start();
    EXPECT_FALSE(errStart);

    // Call Stop to clean up context.
    auto errStop1 = co_await service->Stop();
    EXPECT_FALSE(errStop1);

    EXPECT_EQ(service->GetState(), ServiceBase::State::kNone);

    // Call Stop again on the stopped service. It must be safe and do nothing.
    auto errStop2 = co_await service->Stop();
    EXPECT_FALSE(errStop2);

    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}
