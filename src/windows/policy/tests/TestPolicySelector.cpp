#include <memory>
#include <variant>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "Coroutine.hpp"
#include "Fiber.hpp"
#include "InterfaceWin32.hpp"
#include "Manager.hpp"
#include "PolicyRegistry.hpp"
#include "PolicySelector.hpp"
#include "VpnClientMultiChannel.hpp"
#include "VpnSplitRule.hpp"

using namespace gh;
using namespace gh::policy;

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
  boost::asio::any_io_executor asioExecutor = ioContext.get_executor();
  PolicySelector selector(asioExecutor, reg, std::weak_ptr<VpnSplitRule>{});

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
    const auto& node = selector.GetProcessTreeTracker().AddProcess(1001, 0, pid, "C:\\App\\bypass.exe");

    // 2. Flow establishing Sequence Number
    co_await selector.GetFlowTracker().OnFlowEstablished(FlowTracker::ToFlowExactKey(key).value(), node.ProcessId);

    // 3. Packet arrives
    auto resolved = selector.ResolvePolicy(key);

    // Verification
    EXPECT_TRUE(std::holds_alternative<Interface::PolicyRule::ByPassRoute>(resolved));

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
  boost::asio::any_io_executor asioExecutor = ioContext.get_executor();
  PolicySelector selector(asioExecutor, reg, std::weak_ptr<VpnSplitRule>{});

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
    const auto& node = selector.GetProcessTreeTracker().AddProcess(1001, 0, pid, "C:\\App\\bypass.exe");

    // 2. Flow establishing Sequence Number
    co_await selector.GetFlowTracker().OnFlowEstablished(FlowTracker::ToFlowExactKey(key).value(), node.ProcessId);

    // 3. Packet arrives
    auto resolved = selector.ResolvePolicy(key);

    // Verification
    EXPECT_TRUE(std::holds_alternative<Interface::PolicyRule::ByPassRoute>(resolved));

    testDone = true;
    co_return;
  });

  ioContext.restart();
  ioContext.run();
  EXPECT_TRUE(testDone);
}

// Case 3: Unknown Flow / Packet without Flow mapping defaults to Bypass mark
TEST_F(TestPolicySelector, UnmappedFlowDefaultsToBypass) {
  boost::asio::io_context ioContext;
  Omni::Fiber::AsioExecutor executor(ioContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  PolicyRegistry reg;
  boost::asio::any_io_executor asioExecutor = ioContext.get_executor();
  PolicySelector selector(asioExecutor, reg, std::weak_ptr<VpnSplitRule>{});

  auto localIp = boost::asio::ip::make_address("127.0.0.1").to_v4();
  auto remoteIp = boost::asio::ip::make_address("8.8.8.8").to_v4();
  ConnectionTracker::Ip4TcpKey key{
      .LocalAddress = localIp, .RemoteAddress = remoteIp, .LocalPort = 54321, .RemotePort = 443};

  bool testDone = false;
  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto resolved = selector.ResolvePolicy(key);
    EXPECT_TRUE(std::holds_alternative<Interface::PolicyRule::ByPassRoute>(resolved));

    testDone = true;
    co_return;
  });

  ioContext.restart();
  ioContext.run();
  EXPECT_TRUE(testDone);
}

namespace {

class MockSplitCallback : public VpnSplitRule::Callback {
public:
  MockSplitCallback(uint16_t matchPort, Interface::PolicyRule::RoutingAction action)
      : _MatchPort(matchPort), _Action(std::move(action)) {}

  [[nodiscard]] auto Resolve(const ConnectionTracker::ConnectionKey& key) const
      -> std::optional<Interface::PolicyRule::RoutingAction> override {
    if (const auto* udp4 = std::get_if<ConnectionTracker::Ip4UdpKey>(&key)) {
      if (udp4->LocalPort == _MatchPort) {
        return _Action;
      }
    } else if (const auto* udp6 = std::get_if<ConnectionTracker::Ip6UdpKey>(&key)) {
      if (udp6->LocalPort == _MatchPort) {
        return _Action;
      }
    }
    return std::nullopt;
  }

private:
  uint16_t _MatchPort;
  Interface::PolicyRule::RoutingAction _Action;
};

} // namespace

TEST_F(TestPolicySelector, VpnSplitRouteResolvesTunnelAndDnsCallbacksAndFallsBackToBypass) {
  boost::asio::io_context ioContext;
  Omni::Fiber::AsioExecutor executor(ioContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto splitRule = std::make_shared<VpnSplitRule>();
  PolicyRegistry reg;
  boost::asio::any_io_executor asioExecutor = ioContext.get_executor();
  PolicySelector selector(asioExecutor, reg, splitRule);

  auto session = std::make_shared<VpnClientMultiChannelSession>(UdpDynMux::PskType{}, "vpn-dns-endpoint");
  MockSplitCallback tunnelCallback(40000, PolicyRule::ByPassRoute{});
  MockSplitCallback dnsEndpointCallback(50001, PolicyRule::EndpointRoute{session});
  MockSplitCallback dnsDiscardCallback(50002, PolicyRule::DiscardRoute{});

  splitRule->AddCallback(tunnelCallback);
  splitRule->AddCallback(dnsEndpointCallback);
  splitRule->AddCallback(dnsDiscardCallback);

  PolicyRule vpnRule{.Action = PolicyRule::VpnSplitRoute{.Rules = splitRule}, .Scope = PolicyScope::ProcessSubtree};
  reg.AddPathRule("C:\\App\\vpn.exe", vpnRule);

  auto localIp = boost::asio::ip::make_address("127.0.0.1").to_v4();
  auto remoteIp = boost::asio::ip::make_address("8.8.8.8").to_v4();
  DWORD pid = 4200;

  bool testDone = false;
  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    const auto& node = selector.GetProcessTreeTracker().AddProcess(2001, 0, pid, "C:\\App\\vpn.exe");

    // 1. Tunnel UDP port -> ByPassRoute
    ConnectionTracker::Ip4UdpKey tunnelKey{
        .LocalAddress = localIp, .RemoteAddress = remoteIp, .LocalPort = 40000, .RemotePort = 8443};
    co_await selector.GetFlowTracker().OnFlowEstablished(FlowTracker::ToFlowExactKey(tunnelKey).value(), node.ProcessId);
    auto resolvedTunnel = selector.ResolvePolicy(tunnelKey);
    EXPECT_TRUE(std::holds_alternative<Interface::PolicyRule::ByPassRoute>(resolvedTunnel));

    // 2. DNS upstream port 50001 -> EndpointRoute
    ConnectionTracker::Ip4UdpKey dnsKey1{
        .LocalAddress = localIp, .RemoteAddress = remoteIp, .LocalPort = 50001, .RemotePort = 53};
    co_await selector.GetFlowTracker().OnFlowEstablished(FlowTracker::ToFlowExactKey(dnsKey1).value(), node.ProcessId);
    auto resolvedDns1 = selector.ResolvePolicy(dnsKey1);
    EXPECT_TRUE(std::holds_alternative<Interface::PolicyRule::EndpointRoute>(resolvedDns1));
    if (std::holds_alternative<Interface::PolicyRule::EndpointRoute>(resolvedDns1)) {
      EXPECT_EQ(std::get<Interface::PolicyRule::EndpointRoute>(resolvedDns1).Endpoint.lock(), session);
    }

    // 3. DNS upstream port 50002 -> DiscardRoute
    ConnectionTracker::Ip4UdpKey dnsKey2{
        .LocalAddress = localIp, .RemoteAddress = remoteIp, .LocalPort = 50002, .RemotePort = 53};
    co_await selector.GetFlowTracker().OnFlowEstablished(FlowTracker::ToFlowExactKey(dnsKey2).value(), node.ProcessId);
    auto resolvedDns2 = selector.ResolvePolicy(dnsKey2);
    EXPECT_TRUE(std::holds_alternative<Interface::PolicyRule::DiscardRoute>(resolvedDns2));

    // 4. Unmatched UDP port -> fallback ByPassRoute
    ConnectionTracker::Ip4UdpKey otherUdpKey{
        .LocalAddress = localIp, .RemoteAddress = remoteIp, .LocalPort = 60000, .RemotePort = 123};
    co_await selector.GetFlowTracker().OnFlowEstablished(FlowTracker::ToFlowExactKey(otherUdpKey).value(),
                                                         node.ProcessId);
    auto resolvedOtherUdp = selector.ResolvePolicy(otherUdpKey);
    EXPECT_TRUE(std::holds_alternative<Interface::PolicyRule::ByPassRoute>(resolvedOtherUdp));

    // 5. TCP connection on same port 50001 -> fallback ByPassRoute (since only UDP ports match)
    ConnectionTracker::Ip4TcpKey tcpKey{
        .LocalAddress = localIp, .RemoteAddress = remoteIp, .LocalPort = 50001, .RemotePort = 443};
    co_await selector.GetFlowTracker().OnFlowEstablished(FlowTracker::ToFlowExactKey(tcpKey).value(), node.ProcessId);
    auto resolvedTcp = selector.ResolvePolicy(tcpKey);
    EXPECT_TRUE(std::holds_alternative<Interface::PolicyRule::ByPassRoute>(resolvedTcp));

    // 6. After removing dnsEndpointCallback, port 50001 falls back to ByPassRoute
    splitRule->RemoveCallback(dnsEndpointCallback);
    auto resolvedAfterRemove = selector.ResolvePolicy(dnsKey1);
    EXPECT_TRUE(std::holds_alternative<Interface::PolicyRule::ByPassRoute>(resolvedAfterRemove));

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
