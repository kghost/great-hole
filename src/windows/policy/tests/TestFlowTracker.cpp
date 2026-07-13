#include <memory>
#include <optional>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "Coroutine.hpp"
#include "Fiber.hpp"
#include "FlowTracker.hpp"
#include "Manager.hpp"

using namespace gh;
using namespace gh::policy;

class MockFlowTrackerCallback : public FlowTrackerDeferredCallback {
public:
  auto FlowTrackerContinue(const std::shared_ptr<VpnClientMultiChannel::Mark>& /*mark*/, DWORD /*pid*/)
      -> Omni::Fiber::Coroutine<void> override {
    co_return;
  }
};

class TestFlowTracker : public ::testing::Test {
protected:
  boost::asio::io_context ioContext;
  MockFlowTrackerCallback callback;
  FlowTracker tracker;

  TestFlowTracker() : ioContext(), callback(), tracker(callback) {}

  void SetUp() override {}

  void TearDown() override {}
};

TEST_F(TestFlowTracker, FlowTracking) {
  Omni::Fiber::AsioExecutor executor(ioContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  bool testDone = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    boost::asio::ip::address localIp = boost::asio::ip::make_address("127.0.0.1");
    boost::asio::ip::address remoteIp = boost::asio::ip::make_address("8.8.8.8");
    ConnectionTracker::Ip4TcpKey key{
        .LocalAddress = localIp.to_v4(), .RemoteAddress = remoteIp.to_v4(), .LocalPort = 12345, .RemotePort = 443};

    // Initially no flow
    auto pid = tracker.GetPidForConnection(key);
    EXPECT_FALSE(pid.has_value());

    // Establish flow
    co_await tracker.OnFlowEstablished(key, 1234);
    pid = tracker.GetPidForConnection(key);
    EXPECT_TRUE(pid.has_value());
    if (pid.has_value()) {
      EXPECT_EQ(*pid, 1234);
    }

    // Delete flow
    co_await tracker.OnFlowDeleted(key);
    pid = tracker.GetPidForConnection(key);
    EXPECT_FALSE(pid.has_value());

    testDone = true;
    co_return;
  });

  ioContext.restart();
  ioContext.run();
  EXPECT_TRUE(testDone);
}
