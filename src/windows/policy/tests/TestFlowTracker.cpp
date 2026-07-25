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
  struct ContinuedFlow {
    std::shared_ptr<VpnClientMultiChannel::Mark> Mark;
    DWORD Pid{0};
  };
  std::vector<ContinuedFlow> ContinuedFlows;

  auto FlowTrackerContinue(const std::shared_ptr<VpnClientMultiChannel::Mark>& mark, DWORD pid)
      -> Omni::Fiber::Coroutine<void> override {
    ContinuedFlows.push_back({mark, pid});
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
    co_await tracker.OnFlowEstablished(FlowTracker::ToFlowExactKey(key).value(), 1234);
    pid = tracker.GetPidForConnection(key);
    EXPECT_TRUE(pid.has_value());
    if (pid.has_value()) {
      EXPECT_EQ(*pid, 1234);
    }

    // Delete flow
    co_await tracker.OnFlowDeleted(FlowTracker::ToFlowExactKey(key).value());
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
    co_await tracker.OnFlowEstablished(FlowTracker::ToFlowExactKey(key1).value(), 1001);
    flows = tracker.GetFlows();
    EXPECT_EQ(flows.size(), 1);
    if (!flows.empty()) {
      EXPECT_EQ(flows[0].ProcessId, 1001);
      EXPECT_EQ(flows[0].LocalPort, 11111);
    }

    // Add key2
    co_await tracker.OnFlowEstablished(FlowTracker::ToFlowExactKey(key2).value(), 1002);
    flows = tracker.GetFlows();
    EXPECT_EQ(flows.size(), 2);
    DWORD pid1 = 0;
    DWORD pid2 = 0;
    for (const auto& flow : flows) {
      if (flow.LocalPort == 11111) {
        pid1 = flow.ProcessId;
      } else if (flow.LocalPort == 22222) {
        pid2 = flow.ProcessId;
      }
    }
    EXPECT_EQ(pid1, 1001);
    EXPECT_EQ(pid2, 1002);

    // Delete key1
    co_await tracker.OnFlowDeleted(FlowTracker::ToFlowExactKey(key1).value());
    flows = tracker.GetFlows();
    EXPECT_EQ(flows.size(), 1);
    if (!flows.empty()) {
      EXPECT_EQ(flows[0].ProcessId, 1002);
      EXPECT_EQ(flows[0].LocalPort, 22222);
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
    co_await tracker.OnFlowEstablished(FlowTracker::ToFlowExactKey(key1).value(), 1234);
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
    co_await tracker.OnFlowDeleted(FlowTracker::ToFlowExactKey(key1).value());
    pending = tracker.GetPendingFlows();
    EXPECT_TRUE(pending.empty());

    testDone = true;
    co_return;
  });

  ioContext.restart();
  ioContext.run();
  EXPECT_TRUE(testDone);
}

TEST_F(TestFlowTracker, ExpiredPendingFlowMark) {
  Omni::Fiber::AsioExecutor executor(ioContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  bool testDone = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    boost::asio::ip::address localIp = boost::asio::ip::make_address("127.0.0.1");
    boost::asio::ip::address remoteIp = boost::asio::ip::make_address("8.8.8.8");
    ConnectionTracker::Ip4TcpKey key1{
        .LocalAddress = localIp.to_v4(), .RemoteAddress = remoteIp.to_v4(), .LocalPort = 11111, .RemotePort = 80};

    {
      VpnClientMultiChannel::Mark::Deferred deferred;
      deferred.Packets.push_back(std::make_unique<MockDeferredPacket>(Packet{}));
      auto mark = std::make_shared<VpnClientMultiChannel::Mark>(std::move(deferred));
      tracker.AddPendingMark(key1, mark);
    } // mark is destroyed here

    // Since mark expired, GetPendingFlows should ignore it
    auto pending = tracker.GetPendingFlows();
    EXPECT_TRUE(pending.empty());

    // On flow established, expired mark should safely be ignored
    co_await tracker.OnFlowEstablished(FlowTracker::ToFlowExactKey(key1).value(), 1234);

    testDone = true;
    co_return;
  });

  ioContext.restart();
  ioContext.run();
  EXPECT_TRUE(testDone);
}

TEST_F(TestFlowTracker, WildcardFlowTracking) {
  Omni::Fiber::AsioExecutor executor(ioContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  bool testDone = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    boost::asio::ip::address localIp4 = boost::asio::ip::make_address("192.168.1.100");
    boost::asio::ip::address remoteIp4 = boost::asio::ip::make_address("8.8.8.8");
    ConnectionTracker::Ip4TcpKey key4{
        .LocalAddress = localIp4.to_v4(), .RemoteAddress = remoteIp4.to_v4(), .LocalPort = 33333, .RemotePort = 80};

    // Establish flow bound to 0.0.0.0 (any)
    WinDivertFlowSnifferCallback::FlowIp4Key wildcard4Key{.Proto = WinDivertFlowSnifferCallback::Protocol::Ipv4Tcp,
                                                          .LocalAddress = boost::asio::ip::address_v4::any(),
                                                          .LocalPort = 33333};
    co_await tracker.OnFlowEstablished(wildcard4Key, 5555);

    // Matching exact local address should resolve to pid 5555 via wildcard fallback
    auto pid4 = tracker.GetPidForConnection(key4);
    EXPECT_TRUE(pid4.has_value());
    if (pid4.has_value()) {
      EXPECT_EQ(*pid4, 5555);
    }

    // Establish IPv6 flow bound to :: (any)
    boost::asio::ip::address localIp6 = boost::asio::ip::make_address("2001:db8::1");
    boost::asio::ip::address remoteIp6 = boost::asio::ip::make_address("2001:db8::2");
    ConnectionTracker::Ip6TcpKey key6{
        .LocalAddress = localIp6.to_v6(), .RemoteAddress = remoteIp6.to_v6(), .LocalPort = 44444, .RemotePort = 443};

    WinDivertFlowSnifferCallback::FlowIp6Key wildcard6Key{.Proto = WinDivertFlowSnifferCallback::Protocol::Ipv6Tcp,
                                                          .LocalAddress = boost::asio::ip::address_v6::any(),
                                                          .LocalPort = 44444};
    co_await tracker.OnFlowEstablished(wildcard6Key, 6666);

    auto pid6 = tracker.GetPidForConnection(key6);
    EXPECT_TRUE(pid6.has_value());
    if (pid6.has_value()) {
      EXPECT_EQ(*pid6, 6666);
    }

    testDone = true;
    co_return;
  });

  ioContext.restart();
  ioContext.run();
  EXPECT_TRUE(testDone);
}

TEST_F(TestFlowTracker, WildcardFlowTrackingPendingAfterGetPid) {
  Omni::Fiber::AsioExecutor executor(ioContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  bool testDone = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    boost::asio::ip::address localIp4 = boost::asio::ip::make_address("192.168.1.100");
    boost::asio::ip::address remoteIp4 = boost::asio::ip::make_address("8.8.8.8");
    ConnectionTracker::Ip4TcpKey key4{
        .LocalAddress = localIp4.to_v4(), .RemoteAddress = remoteIp4.to_v4(), .LocalPort = 33333, .RemotePort = 80};

    // 1. GetPidForConnection initially fails (packet arrives before flow established)
    auto pidBefore = tracker.GetPidForConnection(key4);
    EXPECT_FALSE(pidBefore.has_value());

    // 2. Add pending mark for key4
    VpnClientMultiChannel::Mark::Deferred deferred;
    deferred.Packets.push_back(std::make_unique<MockDeferredPacket>(Packet{}));
    auto mark = std::make_shared<VpnClientMultiChannel::Mark>(std::move(deferred));
    tracker.AddPendingMark(key4, mark);

    auto pendingBefore = tracker.GetPendingFlows();
    EXPECT_EQ(pendingBefore.size(), 1);

    // 3. OnFlowEstablished called LATER with a wildcard key (0.0.0.0)
    WinDivertFlowSnifferCallback::FlowIp4Key wildcard4Key{.Proto = WinDivertFlowSnifferCallback::Protocol::Ipv4Tcp,
                                                          .LocalAddress = boost::asio::ip::address_v4::any(),
                                                          .LocalPort = 33333};
    co_await tracker.OnFlowEstablished(wildcard4Key, 7777);

    // 4. Pending mark should be resumed and cleared
    auto pendingAfter = tracker.GetPendingFlows();
    EXPECT_TRUE(pendingAfter.empty());

    EXPECT_EQ(callback.ContinuedFlows.size(), 1);
    if (!callback.ContinuedFlows.empty()) {
      EXPECT_EQ(callback.ContinuedFlows[0].Mark, mark);
      EXPECT_EQ(callback.ContinuedFlows[0].Pid, 7777);
    }

    // 5. Subsequent GetPidForConnection now resolves via wildcard
    auto pidAfter = tracker.GetPidForConnection(key4);
    EXPECT_TRUE(pidAfter.has_value());
    if (pidAfter.has_value()) {
      EXPECT_EQ(*pidAfter, 7777);
    }

    testDone = true;
    co_return;
  });

  ioContext.restart();
  ioContext.run();
  EXPECT_TRUE(testDone);
}

