#include <memory>
#include <variant>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "Coroutine.hpp"
#include "Fiber.hpp"
#include "Manager.hpp"
#include "PolicyRegistry.hpp"
#include "PolicySelector.hpp"

using namespace gh;
using namespace gh::policy;

class MockDeferredPacketInjector : public DeferredPacketInjector {
public:
  std::vector<Packet> InjectedPackets;

  auto Inject(Packet&& packet, const WINDIVERT_ADDRESS& /*addr*/, WinDivertRouteCallback::Result /*route*/)
      -> Omni::Fiber::Coroutine<void> override {
    InjectedPackets.push_back(std::move(packet));
    co_return;
  }

  void Clear() { InjectedPackets.clear(); }
};

class TestPolicySelector : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override {}
};

// Case 1: Best Case (F -> Pr -> P)
TEST_F(TestPolicySelector, OutOfOrder_F_Pr_P) {
  boost::asio::io_context ioContext;
  Omni::Fiber::AsioExecutor executor(ioContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  PolicyRegistry reg;
  MockDeferredPacketInjector injector;
  boost::asio::any_io_executor asioExecutor = ioContext.get_executor();
  PolicySelector selector(asioExecutor, reg);
  selector.SetInjector(injector);

  reg.SetDefaultAction(PolicyRule::EndpointRoute{});
  PolicyRule bypassRule{.Action = PolicyRule::ByPassRoute{}, .Scope = PolicyScope::SingleProcess};
  reg.AddPathRule("C:\\App\\bypass.exe", bypassRule);

  auto localIp = boost::asio::ip::make_address("127.0.0.1").to_v4();
  auto remoteIp = boost::asio::ip::make_address("8.8.8.8").to_v4();
  ConnectionTracker::Ip4TcpKey key{
      .LocalAddress = localIp, .RemoteAddress = remoteIp, .LocalPort = 12345, .RemotePort = 443};
  DWORD pid = 3000;

  bool testDone = false;
  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    // 1. Flow establishing PID
    co_await selector.GetFlowTracker().OnFlowEstablished(FlowTracker::ToFlowExactKey(key).value(), pid);

    // 2. Process starts/policy resolved
    selector.GetProcessTreeTracker().AddProcess(pid, 0, "C:\\App\\bypass.exe");

    // 3. Packet arrives
    auto resolved = selector.ResolvePolicy(key);

    // Verification
    EXPECT_NE(resolved, nullptr);
    auto vpnMark = std::dynamic_pointer_cast<VpnClientMultiChannel::Mark>(resolved);
    EXPECT_NE(vpnMark, nullptr);
    if (vpnMark != nullptr) {
      EXPECT_TRUE(std::holds_alternative<VpnClientMultiChannel::Mark::Bypass>(vpnMark->GetValue()));
    }
    EXPECT_TRUE(injector.InjectedPackets.empty());

    testDone = true;
    co_return;
  });

  ioContext.restart();
  ioContext.run();
  EXPECT_TRUE(testDone);
}

// Case 2: Best Case (Pr -> F -> P)
TEST_F(TestPolicySelector, OutOfOrder_Pr_F_P) {
  boost::asio::io_context ioContext;
  Omni::Fiber::AsioExecutor executor(ioContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  PolicyRegistry reg;
  MockDeferredPacketInjector injector;
  boost::asio::any_io_executor asioExecutor = ioContext.get_executor();
  PolicySelector selector(asioExecutor, reg);
  selector.SetInjector(injector);

  reg.SetDefaultAction(PolicyRule::EndpointRoute{});
  PolicyRule bypassRule{.Action = PolicyRule::ByPassRoute{}, .Scope = PolicyScope::SingleProcess};
  reg.AddPathRule("C:\\App\\bypass.exe", bypassRule);

  auto localIp = boost::asio::ip::make_address("127.0.0.1").to_v4();
  auto remoteIp = boost::asio::ip::make_address("8.8.8.8").to_v4();
  ConnectionTracker::Ip4TcpKey key{
      .LocalAddress = localIp, .RemoteAddress = remoteIp, .LocalPort = 12345, .RemotePort = 443};
  DWORD pid = 3000;

  bool testDone = false;
  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    // 1. Process starts/policy resolved
    selector.GetProcessTreeTracker().AddProcess(pid, 0, "C:\\App\\bypass.exe");

    // 2. Flow establishing PID
    co_await selector.GetFlowTracker().OnFlowEstablished(FlowTracker::ToFlowExactKey(key).value(), pid);

    // 3. Packet arrives
    auto resolved = selector.ResolvePolicy(key);

    // Verification
    EXPECT_NE(resolved, nullptr);
    auto vpnMark = std::dynamic_pointer_cast<VpnClientMultiChannel::Mark>(resolved);
    EXPECT_NE(vpnMark, nullptr);
    if (vpnMark != nullptr) {
      EXPECT_TRUE(std::holds_alternative<VpnClientMultiChannel::Mark::Bypass>(vpnMark->GetValue()));
    }
    EXPECT_TRUE(injector.InjectedPackets.empty());

    testDone = true;
    co_return;
  });

  ioContext.restart();
  ioContext.run();
  EXPECT_TRUE(testDone);
}

// Case 3: Case F -> P -> Pr
TEST_F(TestPolicySelector, OutOfOrder_F_P_Pr) {
  boost::asio::io_context ioContext;
  Omni::Fiber::AsioExecutor executor(ioContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  PolicyRegistry reg;
  MockDeferredPacketInjector injector;
  boost::asio::any_io_executor asioExecutor = ioContext.get_executor();
  PolicySelector selector(asioExecutor, reg);
  selector.SetInjector(injector);

  reg.SetDefaultAction(PolicyRule::EndpointRoute{});
  PolicyRule bypassRule{.Action = PolicyRule::ByPassRoute{}, .Scope = PolicyScope::SingleProcess};
  reg.AddPathRule("C:\\App\\bypass.exe", bypassRule);

  auto localIp = boost::asio::ip::make_address("127.0.0.1").to_v4();
  auto remoteIp = boost::asio::ip::make_address("8.8.8.8").to_v4();
  ConnectionTracker::Ip4TcpKey key{
      .LocalAddress = localIp, .RemoteAddress = remoteIp, .LocalPort = 12345, .RemotePort = 443};
  DWORD pid = 3000;

  bool testDone = false;
  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    // 1. Flow establishing PID
    co_await selector.GetFlowTracker().OnFlowEstablished(FlowTracker::ToFlowExactKey(key).value(), pid);

    // 2. Packet arrives (Process not started yet)
    auto resolved = selector.ResolvePolicy(key);

    // Verification: should be deferred
    EXPECT_NE(resolved, nullptr);
    auto vpnMark = std::dynamic_pointer_cast<VpnClientMultiChannel::Mark>(resolved);
    EXPECT_NE(vpnMark, nullptr);
    if (vpnMark != nullptr) {
      EXPECT_TRUE(std::holds_alternative<VpnClientMultiChannel::Mark::Deferred>(vpnMark->GetValue()));

      if (std::holds_alternative<VpnClientMultiChannel::Mark::Deferred>(vpnMark->GetValue())) {
        // Queue a packet
        Packet packet(100);
        packet.SetMark(vpnMark);
        std::get<VpnClientMultiChannel::Mark::Deferred>(vpnMark->GetValue())
            .Packets.push_back(std::make_unique<WinDivertDeferredPacket>(std::move(packet), WINDIVERT_ADDRESS{}));
      }
    }

    // 3. Process starts/policy resolved (simulated event)
    selector.GetProcessTreeTracker().AddProcess(pid, 0, "C:\\App\\bypass.exe");
    auto action = selector.GetProcessTreeTracker().GetAction(pid);
    EXPECT_TRUE(action.has_value());
    if (action.has_value() && vpnMark != nullptr) {
      co_await selector.ProcessTreeTrackerContinue(vpnMark, action.value());
    }

    // Verification: mark should now be Bypass and the packet should be injected
    if (vpnMark != nullptr) {
      EXPECT_TRUE(std::holds_alternative<VpnClientMultiChannel::Mark::Bypass>(vpnMark->GetValue()));
    }
    EXPECT_EQ(injector.InjectedPackets.size(), 1);
    if (injector.InjectedPackets.size() == 1) {
      EXPECT_TRUE(injector.InjectedPackets[0].HasMark());
      if (vpnMark != nullptr) {
        EXPECT_EQ(&injector.InjectedPackets[0].GetMark(), vpnMark.get());
      }
    }

    testDone = true;
    co_return;
  });

  ioContext.restart();
  ioContext.run();
  EXPECT_TRUE(testDone);
}

// Case 4: Case Pr -> P -> F
TEST_F(TestPolicySelector, OutOfOrder_Pr_P_F) {
  boost::asio::io_context ioContext;
  Omni::Fiber::AsioExecutor executor(ioContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  PolicyRegistry reg;
  MockDeferredPacketInjector injector;
  boost::asio::any_io_executor asioExecutor = ioContext.get_executor();
  PolicySelector selector(asioExecutor, reg);
  selector.SetInjector(injector);

  reg.SetDefaultAction(PolicyRule::EndpointRoute{});
  PolicyRule bypassRule{.Action = PolicyRule::ByPassRoute{}, .Scope = PolicyScope::SingleProcess};
  reg.AddPathRule("C:\\App\\bypass.exe", bypassRule);

  auto localIp = boost::asio::ip::make_address("127.0.0.1").to_v4();
  auto remoteIp = boost::asio::ip::make_address("8.8.8.8").to_v4();
  ConnectionTracker::Ip4TcpKey key{
      .LocalAddress = localIp, .RemoteAddress = remoteIp, .LocalPort = 12345, .RemotePort = 443};
  DWORD pid = 3000;

  bool testDone = false;
  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    // 1. Process starts/policy resolved
    selector.GetProcessTreeTracker().AddProcess(pid, 0, "C:\\App\\bypass.exe");

    // 2. Packet arrives (no flow yet)
    auto resolved = selector.ResolvePolicy(key);

    // Verification: should be deferred
    EXPECT_NE(resolved, nullptr);
    auto vpnMark = std::dynamic_pointer_cast<VpnClientMultiChannel::Mark>(resolved);
    EXPECT_NE(vpnMark, nullptr);
    if (vpnMark != nullptr) {
      EXPECT_TRUE(std::holds_alternative<VpnClientMultiChannel::Mark::Deferred>(vpnMark->GetValue()));

      if (std::holds_alternative<VpnClientMultiChannel::Mark::Deferred>(vpnMark->GetValue())) {
        // Queue a packet
        Packet packet(100);
        packet.SetMark(vpnMark);
        std::get<VpnClientMultiChannel::Mark::Deferred>(vpnMark->GetValue())
            .Packets.push_back(std::make_unique<WinDivertDeferredPacket>(std::move(packet), WINDIVERT_ADDRESS{}));
      }
    }

    // 3. Flow establishes PID (fully automatic continuation trigger)
    co_await selector.GetFlowTracker().OnFlowEstablished(FlowTracker::ToFlowExactKey(key).value(), pid);

    // Verification: mark should now be Bypass and the packet should be injected
    if (vpnMark != nullptr) {
      EXPECT_TRUE(std::holds_alternative<VpnClientMultiChannel::Mark::Bypass>(vpnMark->GetValue()));
    }
    EXPECT_EQ(injector.InjectedPackets.size(), 1);
    if (injector.InjectedPackets.size() == 1) {
      EXPECT_TRUE(injector.InjectedPackets[0].HasMark());
      if (vpnMark != nullptr) {
        EXPECT_EQ(&injector.InjectedPackets[0].GetMark(), vpnMark.get());
      }
    }

    testDone = true;
    co_return;
  });

  ioContext.restart();
  ioContext.run();
  EXPECT_TRUE(testDone);
}

// Case 5: Case P -> F -> Pr
TEST_F(TestPolicySelector, OutOfOrder_P_F_Pr) {
  boost::asio::io_context ioContext;
  Omni::Fiber::AsioExecutor executor(ioContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  PolicyRegistry reg;
  MockDeferredPacketInjector injector;
  boost::asio::any_io_executor asioExecutor = ioContext.get_executor();
  PolicySelector selector(asioExecutor, reg);
  selector.SetInjector(injector);

  reg.SetDefaultAction(PolicyRule::EndpointRoute{});
  PolicyRule bypassRule{.Action = PolicyRule::ByPassRoute{}, .Scope = PolicyScope::SingleProcess};
  reg.AddPathRule("C:\\App\\bypass.exe", bypassRule);

  auto localIp = boost::asio::ip::make_address("127.0.0.1").to_v4();
  auto remoteIp = boost::asio::ip::make_address("8.8.8.8").to_v4();
  ConnectionTracker::Ip4TcpKey key{
      .LocalAddress = localIp, .RemoteAddress = remoteIp, .LocalPort = 12345, .RemotePort = 443};
  DWORD pid = 3000;

  bool testDone = false;
  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    // 1. Packet arrives (no flow, no process)
    auto resolved = selector.ResolvePolicy(key);

    // Verification: should be deferred
    EXPECT_NE(resolved, nullptr);
    auto vpnMark = std::dynamic_pointer_cast<VpnClientMultiChannel::Mark>(resolved);
    EXPECT_NE(vpnMark, nullptr);
    if (vpnMark != nullptr) {
      EXPECT_TRUE(std::holds_alternative<VpnClientMultiChannel::Mark::Deferred>(vpnMark->GetValue()));

      if (std::holds_alternative<VpnClientMultiChannel::Mark::Deferred>(vpnMark->GetValue())) {
        // Queue a packet
        Packet packet(100);
        packet.SetMark(vpnMark);
        std::get<VpnClientMultiChannel::Mark::Deferred>(vpnMark->GetValue())
            .Packets.push_back(std::make_unique<WinDivertDeferredPacket>(std::move(packet), WINDIVERT_ADDRESS{}));
      }
    }

    // 2. Flow establishes PID (process not started yet, so remains deferred)
    co_await selector.GetFlowTracker().OnFlowEstablished(FlowTracker::ToFlowExactKey(key).value(), pid);

    if (vpnMark != nullptr) {
      EXPECT_TRUE(std::holds_alternative<VpnClientMultiChannel::Mark::Deferred>(vpnMark->GetValue()));
    }
    EXPECT_TRUE(injector.InjectedPackets.empty());

    // 3. Process starts/policy resolved (simulated event)
    selector.GetProcessTreeTracker().AddProcess(pid, 0, "C:\\App\\bypass.exe");
    auto action = selector.GetProcessTreeTracker().GetAction(pid);
    EXPECT_TRUE(action.has_value());
    if (action.has_value() && vpnMark != nullptr) {
      co_await selector.ProcessTreeTrackerContinue(vpnMark, action.value());
    }

    // Verification: mark should now be Bypass and the packet should be injected
    if (vpnMark != nullptr) {
      EXPECT_TRUE(std::holds_alternative<VpnClientMultiChannel::Mark::Bypass>(vpnMark->GetValue()));
    }
    EXPECT_EQ(injector.InjectedPackets.size(), 1);
    if (injector.InjectedPackets.size() == 1) {
      EXPECT_TRUE(injector.InjectedPackets[0].HasMark());
      if (vpnMark != nullptr) {
        EXPECT_EQ(&injector.InjectedPackets[0].GetMark(), vpnMark.get());
      }
    }

    testDone = true;
    co_return;
  });

  ioContext.restart();
  ioContext.run();
  EXPECT_TRUE(testDone);
}

// Case 6: Case P -> Pr -> F
TEST_F(TestPolicySelector, OutOfOrder_P_Pr_F) {
  boost::asio::io_context ioContext;
  Omni::Fiber::AsioExecutor executor(ioContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  PolicyRegistry reg;
  MockDeferredPacketInjector injector;
  boost::asio::any_io_executor asioExecutor = ioContext.get_executor();
  PolicySelector selector(asioExecutor, reg);
  selector.SetInjector(injector);

  reg.SetDefaultAction(PolicyRule::EndpointRoute{});
  PolicyRule bypassRule{.Action = PolicyRule::ByPassRoute{}, .Scope = PolicyScope::SingleProcess};
  reg.AddPathRule("C:\\App\\bypass.exe", bypassRule);

  auto localIp = boost::asio::ip::make_address("127.0.0.1").to_v4();
  auto remoteIp = boost::asio::ip::make_address("8.8.8.8").to_v4();
  ConnectionTracker::Ip4TcpKey key{
      .LocalAddress = localIp, .RemoteAddress = remoteIp, .LocalPort = 12345, .RemotePort = 443};
  DWORD pid = 3000;

  bool testDone = false;
  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    // 1. Packet arrives (no flow, no process)
    auto resolved = selector.ResolvePolicy(key);

    // Verification: should be deferred
    EXPECT_NE(resolved, nullptr);
    auto vpnMark = std::dynamic_pointer_cast<VpnClientMultiChannel::Mark>(resolved);
    EXPECT_NE(vpnMark, nullptr);
    if (vpnMark != nullptr) {
      EXPECT_TRUE(std::holds_alternative<VpnClientMultiChannel::Mark::Deferred>(vpnMark->GetValue()));

      if (std::holds_alternative<VpnClientMultiChannel::Mark::Deferred>(vpnMark->GetValue())) {
        // Queue a packet
        Packet packet(100);
        packet.SetMark(vpnMark);
        std::get<VpnClientMultiChannel::Mark::Deferred>(vpnMark->GetValue())
            .Packets.push_back(std::make_unique<WinDivertDeferredPacket>(std::move(packet), WINDIVERT_ADDRESS{}));
      }
    }

    // 2. Process starts/policy resolved
    selector.GetProcessTreeTracker().AddProcess(pid, 0, "C:\\App\\bypass.exe");

    if (vpnMark != nullptr) {
      EXPECT_TRUE(std::holds_alternative<VpnClientMultiChannel::Mark::Deferred>(vpnMark->GetValue()));
    }
    EXPECT_TRUE(injector.InjectedPackets.empty());

    // 3. Flow establishes PID (fully automatic continuation trigger)
    co_await selector.GetFlowTracker().OnFlowEstablished(FlowTracker::ToFlowExactKey(key).value(), pid);

    // Verification: mark should now be Bypass and the packet should be injected
    if (vpnMark != nullptr) {
      EXPECT_TRUE(std::holds_alternative<VpnClientMultiChannel::Mark::Bypass>(vpnMark->GetValue()));
    }
    EXPECT_EQ(injector.InjectedPackets.size(), 1);
    if (injector.InjectedPackets.size() == 1) {
      EXPECT_TRUE(injector.InjectedPackets[0].HasMark());
      if (vpnMark != nullptr) {
        EXPECT_EQ(&injector.InjectedPackets[0].GetMark(), vpnMark.get());
      }
    }

    testDone = true;
    co_return;
  });

  ioContext.restart();
  ioContext.run();
  EXPECT_TRUE(testDone);
}

TEST_F(TestPolicySelector, GetConnectionsTest) {
  boost::asio::io_context ioContext;
  Omni::Fiber::AsioExecutor executor(ioContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  PolicyRegistry reg;
  boost::asio::any_io_executor asioExecutor = ioContext.get_executor();
  PolicySelector selector(asioExecutor, reg);

  EXPECT_TRUE(selector.GetConnections().empty());

  auto tracker = std::make_shared<ConnectionTracker>(ioContext.get_executor());
  selector.SetConnectionTracker(tracker);

  EXPECT_TRUE(selector.GetConnections().empty());

  bool testDone = false;
  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    static const std::vector<uint8_t> ip4TcpSyn = {
        0x45, 0x00, 0x00, 0x3c, 0x46, 0x10, 0x40, 0x00, 0x40, 0x06, 0xe0, 0xff, 0x7f, 0x00, 0x00,
        0x01, 0x7f, 0x59, 0x15, 0x52, 0x83, 0xb8, 0xd4, 0x31, 0xd5, 0xdb, 0x43, 0x66, 0x00, 0x00,
        0x00, 0x00, 0xa0, 0x02, 0xff, 0xd7, 0x13, 0xdb, 0x00, 0x00, 0x02, 0x04, 0xff, 0xd7, 0x04,
        0x02, 0x08, 0x0a, 0x9a, 0x29, 0xbf, 0x4e, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x03, 0x07};

    Packet packet(ip4TcpSyn.size());
    std::copy(ip4TcpSyn.begin(), ip4TcpSyn.end(), packet.Data().begin());

    tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(packet, selector);

    auto connections = selector.GetConnections();
    EXPECT_EQ(connections.size(), 1);
    if (!connections.empty()) {
      EXPECT_EQ(connections[0].Connection.Protocol, "TCPv4");
      EXPECT_EQ(connections[0].Connection.LocalAddress, "127.0.0.1");
      EXPECT_EQ(connections[0].Connection.RemoteAddress, "127.89.21.82");
      EXPECT_EQ(connections[0].Connection.LocalPort, 33720);
      EXPECT_EQ(connections[0].Connection.RemotePort, 54321);
      EXPECT_EQ(connections[0].Mark, "Deferred");
    }

    testDone = true;
    co_return;
  });

  ioContext.restart();
  ioContext.run();
  EXPECT_TRUE(testDone);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
