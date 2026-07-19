#include <memory>
#include <variant>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "PolicyRegistry.hpp"
#include "ProcessTreeTracker.hpp"
#include "VpnClientMultiChannel.hpp"

using namespace gh;
using namespace gh::policy;

class TestProcessTreeTracker : public ::testing::Test, public ProcessTreeTrackerDeferredCallback {
protected:
  boost::asio::io_context ioContext;
  PolicyRegistry registry;
  ProcessTreeTracker tracker;

  TestProcessTreeTracker() : ioContext(), registry(), tracker(ioContext.get_executor(), *this, registry) {}

  auto ProcessTreeTrackerContinue(const std::shared_ptr<VpnClientMultiChannel::Mark>& /*mark*/,
                                  const PolicyRule::RoutingAction& /*action*/)
      -> Omni::Fiber::Coroutine<void> override {
    co_return;
  }

  void RunPending() {
    ioContext.restart();
    ioContext.poll();
  }

  void SetUp() override {
    registry.Clear();
    tracker.ClearAllMock();
    RunPending();
  }

  void TearDown() override {
    registry.Clear();
    tracker.ClearAllMock();
    RunPending();
  }
};

TEST_F(TestProcessTreeTracker, ProcessTreeInheritance) {
  PolicyRegistry& reg = registry;

  auto session99 = std::make_shared<VpnClientMultiChannelSession>();
  PolicyRule subtreeRule{.Action = PolicyRule::EndpointRoute{session99}, .Scope = PolicyScope::ProcessSubtree};
  reg.AddPathRule("C:\\VSCode\\code.exe", subtreeRule);

  tracker.AddProcess(1000, 0, "C:\\VSCode\\code.exe");
  auto policy1000 = tracker.GetPolicy(1000);
  ASSERT_TRUE(policy1000.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(policy1000->Action));
  EXPECT_EQ(std::get<PolicyRule::EndpointRoute>(policy1000->Action).Endpoint.lock(), session99);

  tracker.AddProcess(1001, 1000, "C:\\Windows\\System32\\cmd.exe");
  auto policy1001 = tracker.GetPolicy(1001);
  ASSERT_TRUE(policy1001.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(policy1001->Action));
  EXPECT_EQ(std::get<PolicyRule::EndpointRoute>(policy1001->Action).Endpoint.lock(), session99);

  tracker.AddProcess(1002, 1001, "C:\\Git\\bin\\git.exe");
  auto policy1002 = tracker.GetPolicy(1002);
  ASSERT_TRUE(policy1002.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(policy1002->Action));
  EXPECT_EQ(std::get<PolicyRule::EndpointRoute>(policy1002->Action).Endpoint.lock(), session99);
}

TEST_F(TestProcessTreeTracker, ProcessTreeSingleProcessScopeNoInheritance) {
  PolicyRegistry& reg = registry;

  PolicyRule singleRule{.Action = PolicyRule::ByPassRoute{}, .Scope = PolicyScope::SingleProcess};
  reg.AddPathRule("C:\\App\\app.exe", singleRule);

  tracker.AddProcess(2000, 0, "C:\\App\\app.exe");
  auto policy2000 = tracker.GetPolicy(2000);
  ASSERT_TRUE(policy2000.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::ByPassRoute>(policy2000->Action));

  tracker.AddProcess(2001, 2000, "C:\\Windows\\System32\\cmd.exe");
  auto policy2001 = tracker.GetPolicy(2001);
  EXPECT_FALSE(policy2001.has_value());
}

TEST_F(TestProcessTreeTracker, SubtreeParentExitedButNewDescendantSpawns) {
  PolicyRegistry& reg = registry;

  auto session42 = std::make_shared<VpnClientMultiChannelSession>();
  PolicyRule subtreeRule{.Action = PolicyRule::EndpointRoute{session42}, .Scope = PolicyScope::ProcessSubtree};
  reg.AddPathRule("C:\\App\\parent.exe", subtreeRule);

  // 1. Parent starts (PID 4000)
  tracker.AddProcess(4000, 0, "C:\\App\\parent.exe");

  // 2. Parent spawns Child 1 (PID 4001)
  tracker.AddProcess(4001, 4000, "C:\\App\\child1.exe");
  auto policy4001 = tracker.GetPolicy(4001);
  ASSERT_TRUE(policy4001.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(policy4001->Action));

  // 3. Parent exits (PID 4000 is removed)
  tracker.RemoveProcess(4000);

  // 4. Child 1 spawns Child 2 (PID 4002) after parent exited
  tracker.AddProcess(4002, 4001, "C:\\App\\child2.exe");
  auto policy4002 = tracker.GetPolicy(4002);
  ASSERT_TRUE(policy4002.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(policy4002->Action));
  EXPECT_EQ(std::get<PolicyRule::EndpointRoute>(policy4002->Action).Endpoint.lock(), session42);
}

TEST_F(TestProcessTreeTracker, ProcessTreePolicyCascading) {
  // 1. Setup mock processes with parent-child links: A(1000) -> B(1001) -> C(1002)
  tracker.AddProcess(1000, 0, "C:\\App\\parent.exe");
  tracker.AddProcess(1001, 1000, "C:\\App\\child.exe");
  tracker.AddProcess(1002, 1001, "C:\\App\\grandchild.exe");

  EXPECT_FALSE(tracker.GetPolicy(1000).has_value());
  EXPECT_FALSE(tracker.GetPolicy(1001).has_value());
  EXPECT_FALSE(tracker.GetPolicy(1002).has_value());

  // 2. Register subtree policy on parent A
  auto session88 = std::make_shared<VpnClientMultiChannelSession>();
  PolicyRule subtreeRule{.Action = PolicyRule::EndpointRoute{session88}, .Scope = PolicyScope::ProcessSubtree};
  tracker.RegisterPidPolicy(1000, subtreeRule);
  RunPending();

  // Verify all inherited the policy because of cascading
  auto policyA = tracker.GetPolicy(1000);
  auto policyB = tracker.GetPolicy(1001);
  auto policyC = tracker.GetPolicy(1002);

  ASSERT_TRUE(policyA.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(policyA->Action));
  EXPECT_EQ(std::get<PolicyRule::EndpointRoute>(policyA->Action).Endpoint.lock(), session88);

  ASSERT_TRUE(policyB.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(policyB->Action));
  EXPECT_EQ(std::get<PolicyRule::EndpointRoute>(policyB->Action).Endpoint.lock(), session88);

  ASSERT_TRUE(policyC.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(policyC->Action));
  EXPECT_EQ(std::get<PolicyRule::EndpointRoute>(policyC->Action).Endpoint.lock(), session88);
}

TEST_F(TestProcessTreeTracker, ProcessTreeReparentingOnExit) {
  // 1. Setup mock processes: A(5000) -> B(5001) -> C(5002)
  tracker.AddProcess(5000, 0, "C:\\App\\grandparent.exe");
  tracker.AddProcess(5001, 5000, "C:\\App\\parent.exe");
  tracker.AddProcess(5002, 5001, "C:\\App\\child.exe");

  // 2. Parent B (5001) exits
  tracker.RemoveProcess(5001);

  // 3. Simulate PID reuse: a new unrelated process takes over B's PID (5001)
  // and has a subtree policy
  tracker.AddProcess(5001, 0, "C:\\App\\unrelated.exe");
  auto session77 = std::make_shared<VpnClientMultiChannelSession>();
  PolicyRule unrelatedSubtree{.Action = PolicyRule::EndpointRoute{session77}, .Scope = PolicyScope::ProcessSubtree};
  tracker.RegisterPidPolicy(5001, unrelatedSubtree);

  // 4. Force re-evaluation of Child C (5002)
  tracker.TestReEvaluatePolicy(5002);

  // Verify Child C is unaffected by the new process reusing PID 5001 (reparented to 0)
  auto policyC = tracker.GetPolicy(5002);
  EXPECT_FALSE(policyC.has_value());
}

TEST_F(TestProcessTreeTracker, ExposeProcessTree) {
  tracker.AddProcess(6000, 0, "C:\\App\\grandparent.exe");
  tracker.AddProcess(6001, 6000, "C:\\App\\parent.exe");

  auto session = std::make_shared<VpnClientMultiChannelSession>();
  PolicyRule rule{.Action = PolicyRule::EndpointRoute{session}, .Scope = PolicyScope::ProcessSubtree};
  tracker.RegisterPidPolicy(6000, rule);

  auto tree = tracker.GetProcessTree();
  ASSERT_EQ(tree.size(), 2);

  bool found6000 = false;
  bool found6001 = false;
  for (const auto& entry : tree) {
    if (entry.ProcessId == 6000) {
      found6000 = true;
      EXPECT_EQ(entry.ParentProcessId, 0);
      ASSERT_TRUE(entry.Policy.has_value());
      EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(entry.Policy->Action));
      EXPECT_EQ(entry.Policy->Scope, PolicyScope::ProcessSubtree);
    } else if (entry.ProcessId == 6001) {
      found6001 = true;
      EXPECT_EQ(entry.ParentProcessId, 6000);
      ASSERT_TRUE(entry.Policy.has_value());
      EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(entry.Policy->Action));
      EXPECT_EQ(entry.Policy->Scope, PolicyScope::ProcessSubtree);
    }
  }
  EXPECT_TRUE(found6000);
  EXPECT_TRUE(found6001);
}
