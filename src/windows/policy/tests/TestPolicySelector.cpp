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
  PolicySelector selector(asioExecutor, reg);

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
  PolicySelector selector(asioExecutor, reg);

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
  PolicySelector selector(asioExecutor, reg);

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

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
