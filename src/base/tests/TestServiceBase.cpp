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

  std::string GetName() const override { return "MockService"; }

protected:
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override { co_return ErrorCode{}; }

  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override { co_return ErrorCode{}; }
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

    // Call Stop and WaitService to clean up context.
    auto errStop1 = co_await service->Stop();
    EXPECT_FALSE(errStop1);
    co_await service->WaitService();

    EXPECT_EQ(service->GetState(), ServiceBase::State::kNone);

    // Call Stop again on the stopped service. It must be safe and return ErrorCode{}.
    auto errStop2 = co_await service->Stop();
    EXPECT_FALSE(errStop2);

    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}
