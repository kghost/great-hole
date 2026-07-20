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

struct MockDeferredPacket : public VpnClientMultiChannel::Mark::Deferred::DeferredPacket {
  Packet Pkt;
  MockDeferredPacket(Packet pkt) : Pkt(std::move(pkt)) {}
};

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

TEST_F(TestFlowTracker, GetFlows) {
  Omni::Fiber::AsioExecutor executor(ioContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  bool testDone = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    boost::asio::ip::address localIp = boost::asio::ip::make_address("127.0.0.1");
    boost::asio::ip::address remoteIp = boost::asio::ip::make_address("8.8.8.8");
    ConnectionTracker::Ip4TcpKey key1{
        .LocalAddress = localIp.to_v4(), .RemoteAddress = remoteIp.to_v4(), .LocalPort = 11111, .RemotePort = 80};
    ConnectionTracker::Ip4TcpKey key2{
        .LocalAddress = localIp.to_v4(), .RemoteAddress = remoteIp.to_v4(), .LocalPort = 22222, .RemotePort = 443};

    // No flows initially
    auto flows = tracker.GetFlows();
    EXPECT_TRUE(flows.empty());

    // Add key1
    co_await tracker.OnFlowEstablished(key1, 1001);
    flows = tracker.GetFlows();
    EXPECT_EQ(flows.size(), 1);
    if (!flows.empty()) {
      EXPECT_EQ(flows[0].ProcessId, 1001);
      EXPECT_EQ(flows[0].Connection.LocalPort, 11111);
    }

    // Add key2
    co_await tracker.OnFlowEstablished(key2, 1002);
    flows = tracker.GetFlows();
    EXPECT_EQ(flows.size(), 2);
    DWORD pid1 = 0;
    DWORD pid2 = 0;
    for (const auto& flow : flows) {
      if (flow.Connection.LocalPort == 11111) {
        pid1 = flow.ProcessId;
      } else if (flow.Connection.LocalPort == 22222) {
        pid2 = flow.ProcessId;
      }
    }
    EXPECT_EQ(pid1, 1001);
    EXPECT_EQ(pid2, 1002);

    // Delete key1
    co_await tracker.OnFlowDeleted(key1);
    flows = tracker.GetFlows();
    EXPECT_EQ(flows.size(), 1);
    if (!flows.empty()) {
      EXPECT_EQ(flows[0].ProcessId, 1002);
      EXPECT_EQ(flows[0].Connection.LocalPort, 22222);
    }

    testDone = true;
    co_return;
  });

  ioContext.restart();
  ioContext.run();
  EXPECT_TRUE(testDone);
}

TEST_F(TestFlowTracker, GetPendingFlows) {
  Omni::Fiber::AsioExecutor executor(ioContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  bool testDone = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    boost::asio::ip::address localIp = boost::asio::ip::make_address("127.0.0.1");
    boost::asio::ip::address remoteIp = boost::asio::ip::make_address("8.8.8.8");
    ConnectionTracker::Ip4TcpKey key1{
        .LocalAddress = localIp.to_v4(), .RemoteAddress = remoteIp.to_v4(), .LocalPort = 11111, .RemotePort = 80};

    // Initially no pending flows
    auto pending = tracker.GetPendingFlows();
    EXPECT_TRUE(pending.empty());

    VpnClientMultiChannel::Mark::Deferred deferred;
    deferred.Packets.push_back(std::make_unique<MockDeferredPacket>(Packet{}));
    deferred.Packets.push_back(std::make_unique<MockDeferredPacket>(Packet{}));
    auto mark = std::make_shared<VpnClientMultiChannel::Mark>(std::move(deferred));
    tracker.AddPendingMark(key1, mark);

    pending = tracker.GetPendingFlows();
    EXPECT_EQ(pending.size(), 1);
    if (!pending.empty()) {
      EXPECT_EQ(pending[0].QueueSize.value_or(0), 2);
      EXPECT_EQ(pending[0].Connection.LocalPort, 11111);
    }

    // On flow established, pending mark is resumed and removed
    co_await tracker.OnFlowEstablished(key1, 1234);
    pending = tracker.GetPendingFlows();
    EXPECT_TRUE(pending.empty());

    testDone = true;
    co_return;
  });

  ioContext.restart();
  ioContext.run();
  EXPECT_TRUE(testDone);
}

TEST_F(TestFlowTracker, DeletePendingFlow) {
  Omni::Fiber::AsioExecutor executor(ioContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  bool testDone = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    boost::asio::ip::address localIp = boost::asio::ip::make_address("127.0.0.1");
    boost::asio::ip::address remoteIp = boost::asio::ip::make_address("8.8.8.8");
    ConnectionTracker::Ip4TcpKey key1{
        .LocalAddress = localIp.to_v4(), .RemoteAddress = remoteIp.to_v4(), .LocalPort = 11111, .RemotePort = 80};

    // Initially no pending flows
    auto pending = tracker.GetPendingFlows();
    EXPECT_TRUE(pending.empty());

    VpnClientMultiChannel::Mark::Deferred deferred;
    deferred.Packets.push_back(std::make_unique<MockDeferredPacket>(Packet{}));
    deferred.Packets.push_back(std::make_unique<MockDeferredPacket>(Packet{}));
    auto mark = std::make_shared<VpnClientMultiChannel::Mark>(std::move(deferred));
    tracker.AddPendingMark(key1, mark);

    pending = tracker.GetPendingFlows();
    EXPECT_EQ(pending.size(), 1);

    // On flow deleted, pending mark is removed
    co_await tracker.OnFlowDeleted(key1);
    pending = tracker.GetPendingFlows();
    EXPECT_TRUE(pending.empty());

    testDone = true;
    co_return;
  });

  ioContext.restart();
  ioContext.run();
  EXPECT_TRUE(testDone);
}
