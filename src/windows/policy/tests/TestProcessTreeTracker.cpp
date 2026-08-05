#include <algorithm>
#include <memory>
#include <variant>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include <evntcons.h>
#include <evntrace.h>
#include <windows.h>

#include "PolicyRegistry.hpp"
#include "Process.hpp"
#include "ProcessTreeTracker.hpp"
#include "VpnClientMultiChannel.hpp"

using namespace gh;
using namespace gh::policy;

class TestProcessTreeTracker : public ::testing::Test {
protected:
  boost::asio::io_context ioContext;
  PolicyRegistry registry;
  ProcessTreeTracker tracker;

  TestProcessTreeTracker() : ioContext(), registry(), tracker(ioContext.get_executor(), registry) {}

  void RunPending() {
    ioContext.restart();
    ioContext.poll();
  }

  void CallHandleEtwEvent(PEVENT_RECORD record) { tracker.HandleEtwEvent(record); }

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

  auto HasPolicy(DWORD pid) const -> bool {
    auto tree = tracker.GetProcessTree();
    auto it = std::find_if(tree.begin(), tree.end(), [pid](const auto& info) { return info.InfoProcessId == pid; });
    return it != tree.end() && it->Policy.has_value();
  }
};

TEST_F(TestProcessTreeTracker, ProcessTreeInheritance) {
  PolicyRegistry& reg = registry;

  auto session99 = std::make_shared<VpnClientMultiChannelSession>(UdpDynMux::PskType{}, "dummy");
  PolicyRule subtreeRule{.Action = PolicyRule::EndpointRoute{session99}, .Scope = PolicyScope::ProcessSubtree};
  reg.AddPathRule("C:\\VSCode\\code.exe", subtreeRule);

  const auto& p1000 = tracker.AddProcess(1000, 0, 1000, "C:\\VSCode\\code.exe");
  auto action1000 = tracker.GetAction(p1000.ProcessId);
  ASSERT_TRUE(action1000.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(action1000.value()));
  EXPECT_EQ(std::get<PolicyRule::EndpointRoute>(action1000.value()).Endpoint.lock(), session99);

  const auto& p1001 = tracker.AddProcess(1001, 1000, 1001, "C:\\Windows\\System32\\cmd.exe");
  auto action1001 = tracker.GetAction(p1001.ProcessId);
  ASSERT_TRUE(action1001.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(action1001.value()));
  EXPECT_EQ(std::get<PolicyRule::EndpointRoute>(action1001.value()).Endpoint.lock(), session99);

  const auto& p1002 = tracker.AddProcess(1002, 1001, 1002, "C:\\Git\\bin\\git.exe");
  auto action1002 = tracker.GetAction(p1002.ProcessId);
  ASSERT_TRUE(action1002.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(action1002.value()));
  EXPECT_EQ(std::get<PolicyRule::EndpointRoute>(action1002.value()).Endpoint.lock(), session99);
}

TEST_F(TestProcessTreeTracker, ProcessTreeSingleProcessScopeNoInheritance) {
  PolicyRegistry& reg = registry;

  PolicyRule singleRule{.Action = PolicyRule::ByPassRoute{}, .Scope = PolicyScope::SingleProcess};
  reg.AddPathRule("C:\\App\\app.exe", singleRule);

  const auto& p2000 = tracker.AddProcess(2000, 0, 2000, "C:\\App\\app.exe");
  auto action2000 = tracker.GetAction(p2000.ProcessId);
  ASSERT_TRUE(action2000.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::ByPassRoute>(action2000.value()));

  tracker.AddProcess(2001, 2000, 2001, "C:\\Windows\\System32\\cmd.exe");
  EXPECT_FALSE(HasPolicy(2001));
}

TEST_F(TestProcessTreeTracker, SubtreeParentExitedButNewDescendantSpawns) {
  PolicyRegistry& reg = registry;

  auto session42 = std::make_shared<VpnClientMultiChannelSession>(UdpDynMux::PskType{}, "dummy");
  PolicyRule subtreeRule{.Action = PolicyRule::EndpointRoute{session42}, .Scope = PolicyScope::ProcessSubtree};
  reg.AddPathRule("C:\\App\\parent.exe", subtreeRule);

  // 1. Parent starts (PID 4000)
  const auto& p4000 = tracker.AddProcess(4000, 0, 4000, "C:\\App\\parent.exe");

  // 2. Parent spawns Child 1 (PID 4001)
  const auto& p4001 = tracker.AddProcess(4001, 4000, 4001, "C:\\App\\child1.exe");
  auto action4001 = tracker.GetAction(p4001.ProcessId);
  ASSERT_TRUE(action4001.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(action4001.value()));

  // 3. Parent exits (PID 4000 is removed)
  tracker.RemoveProcess(p4000.ProcessSequence);

  // 4. Child 1 spawns Child 2 (PID 4002) after parent exited
  const auto& p4002 = tracker.AddProcess(4002, 4001, 4002, "C:\\App\\child2.exe");
  auto action4002 = tracker.GetAction(p4002.ProcessId);
  ASSERT_TRUE(action4002.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(action4002.value()));
  EXPECT_EQ(std::get<PolicyRule::EndpointRoute>(action4002.value()).Endpoint.lock(), session42);
}

TEST_F(TestProcessTreeTracker, ProcessTreePolicyCascading) {
  // 1. Setup mock processes with parent-child links: A(1000) -> B(1001) -> C(1002)
  const auto& p1000 = tracker.AddProcess(1000, 0, 1000, "C:\\App\\parent.exe");
  const auto& p1001 = tracker.AddProcess(1001, 1000, 1001, "C:\\App\\child.exe");
  const auto& p1002 = tracker.AddProcess(1002, 1001, 1002, "C:\\App\\grandchild.exe");

  EXPECT_FALSE(HasPolicy(1000));
  EXPECT_FALSE(HasPolicy(1001));
  EXPECT_FALSE(HasPolicy(1002));

  // 2. Register subtree policy on parent A
  auto session88 = std::make_shared<VpnClientMultiChannelSession>(UdpDynMux::PskType{}, "dummy");
  PolicyRule subtreeRule{.Action = PolicyRule::EndpointRoute{session88}, .Scope = PolicyScope::ProcessSubtree};
  tracker.RegisterProcessPolicy(p1000.ProcessSequence, subtreeRule);
  RunPending();

  // Verify all inherited the policy because of cascading
  auto actionA = tracker.GetAction(p1000.ProcessId);
  auto actionB = tracker.GetAction(p1001.ProcessId);
  auto actionC = tracker.GetAction(p1002.ProcessId);

  ASSERT_TRUE(actionA.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(actionA.value()));
  EXPECT_EQ(std::get<PolicyRule::EndpointRoute>(actionA.value()).Endpoint.lock(), session88);

  ASSERT_TRUE(actionB.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(actionB.value()));
  EXPECT_EQ(std::get<PolicyRule::EndpointRoute>(actionB.value()).Endpoint.lock(), session88);

  ASSERT_TRUE(actionC.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(actionC.value()));
  EXPECT_EQ(std::get<PolicyRule::EndpointRoute>(actionC.value()).Endpoint.lock(), session88);
}

TEST_F(TestProcessTreeTracker, ExposeProcessTree) {
  const auto& node6000 = tracker.AddProcess(6000, 0, 6000, "C:\\App\\grandparent.exe");
  const auto& node6001 = tracker.AddProcess(6001, 6000, 6001, "C:\\App\\parent.exe");

  auto session = std::make_shared<VpnClientMultiChannelSession>(UdpDynMux::PskType{}, "dummy");
  PolicyRule rule{.Action = PolicyRule::EndpointRoute{session}, .Scope = PolicyScope::ProcessSubtree};
  tracker.RegisterProcessPolicy(node6000.ProcessSequence, rule);

  auto tree = tracker.GetProcessTree();
  ASSERT_EQ(tree.size(), 2);

  bool found6000 = false;
  bool found6001 = false;
  for (const auto& entry : tree) {
    if (entry.InfoProcessId == 6000) {
      found6000 = true;
      EXPECT_EQ(entry.Process, node6000.ProcessSequence);
      EXPECT_EQ(entry.ParentProcess, 0);
      ASSERT_TRUE(entry.Policy.has_value());
      EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(entry.Policy->Action));
      EXPECT_EQ(entry.Policy->Scope, PolicyScope::ProcessSubtree);
    } else if (entry.InfoProcessId == 6001) {
      found6001 = true;
      EXPECT_EQ(entry.Process, node6001.ProcessSequence);
      EXPECT_EQ(entry.ParentProcess, node6000.ProcessSequence);
      ASSERT_TRUE(entry.Policy.has_value());
      EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(entry.Policy->Action));
      EXPECT_EQ(entry.Policy->Scope, PolicyScope::ProcessSubtree);
    }
  }
  EXPECT_TRUE(found6000);
  EXPECT_TRUE(found6001);
}

TEST_F(TestProcessTreeTracker, QueryActionForLiveProcessByOpenProcess) {
  DWORD currentPid = GetCurrentProcessId();
  HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, currentPid);
  ASSERT_NE(hProcess, nullptr);
  auto pathOpt = gh::GetProcessPath(hProcess);
  CloseHandle(hProcess);
  ASSERT_TRUE(pathOpt.has_value());

  PolicyRule rule{.Action = PolicyRule::ByPassRoute{}, .Scope = PolicyScope::SingleProcess};
  registry.AddPathRule(pathOpt.value(), rule);

  auto action = tracker.GetAction(currentPid);
  ASSERT_TRUE(action.has_value());
}

TEST_F(TestProcessTreeTracker, HandleEtwEventParseImagePath) {
  PolicyRule rule{.Action = PolicyRule::ByPassRoute{}, .Scope = PolicyScope::SingleProcess};
  registry.AddPathRule("C:\\TestApp\\test.exe", rule);

  std::vector<uint8_t> payload;

  auto appendU32 = [&](uint32_t val) {
    auto p = reinterpret_cast<const uint8_t*>(&val);
    payload.insert(payload.end(), p, p + sizeof(val));
  };
  auto appendU64 = [&](uint64_t val) {
    auto p = reinterpret_cast<const uint8_t*>(&val);
    payload.insert(payload.end(), p, p + sizeof(val));
  };

  appendU32(5000);   // ProcessID
  appendU64(50000);  // ProcessSequenceNumber
  appendU64(100000); // CreateTime
  appendU32(1000);   // ParentProcessID
  appendU64(10000);  // ParentProcessSequenceNumber
  appendU32(1);      // SessionID
  appendU32(0);      // Flags
  appendU32(2);      // ProcessTokenElevationType
  appendU32(1);      // ProcessTokenIsElevated

  // MandatoryLabel SID (12 bytes: Rev=1, SubAuthorityCount=1, IdentifierAuthority={0,0,0,0,0,16}, SubAuthority[0]=8192)
  payload.push_back(1); // Revision
  payload.push_back(1); // SubAuthorityCount
  payload.insert(payload.end(), {0, 0, 0, 0, 0, 16});
  appendU32(8192);

  std::wstring imageName = L"C:\\TestApp\\test.exe";
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto wbytes = reinterpret_cast<const uint8_t*>(imageName.c_str());
  payload.insert(payload.end(), wbytes, wbytes + (imageName.size() + 1) * sizeof(wchar_t));

  EVENT_RECORD record{};
  record.EventHeader.ProviderId = {0x22fb2cd6, 0x0e7b, 0x422b, {0xa0, 0xc7, 0x2f, 0xad, 0x1f, 0xd0, 0xe7, 0x16}};
  record.EventHeader.EventDescriptor.Id = 1;
  record.EventHeader.EventDescriptor.Version = 3;
  record.UserData = payload.data();
  record.UserDataLength = static_cast<USHORT>(payload.size());

  CallHandleEtwEvent(&record);
  RunPending();

  auto action = tracker.GetAction(5000);
  ASSERT_TRUE(action.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::ByPassRoute>(action.value()));
}

TEST_F(TestProcessTreeTracker, ApplyPathRuleUpdatesExistingProcessesSubtree) {
  const auto& p1000 = tracker.AddProcess(1000, 0, 1000, "C:\\App\\app.exe");
  const auto& p1001 = tracker.AddProcess(1001, 1000, 1001, "C:\\App\\child.exe");

  EXPECT_FALSE(HasPolicy(1000));
  EXPECT_FALSE(HasPolicy(1001));

  auto session = std::make_shared<VpnClientMultiChannelSession>(UdpDynMux::PskType{}, "dummy");
  PolicyRule subtreeRule{.Action = PolicyRule::EndpointRoute{session}, .Scope = PolicyScope::ProcessSubtree};

  registry.AddPathRule("C:\\App\\app.exe", subtreeRule);
  tracker.ApplyPathRule("C:\\App\\app.exe", subtreeRule);

  auto action1000 = tracker.GetAction(p1000.ProcessId);
  ASSERT_TRUE(action1000.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(action1000.value()));
  EXPECT_EQ(std::get<PolicyRule::EndpointRoute>(action1000.value()).Endpoint.lock(), session);

  auto action1001 = tracker.GetAction(p1001.ProcessId);
  ASSERT_TRUE(action1001.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(action1001.value()));
  EXPECT_EQ(std::get<PolicyRule::EndpointRoute>(action1001.value()).Endpoint.lock(), session);
}

TEST_F(TestProcessTreeTracker, ApplyPathRuleUpdatesExistingProcessesSingleProcess) {
  const auto& p2000 = tracker.AddProcess(2000, 0, 2000, "C:\\App\\app2.exe");
  const auto& p2001 = tracker.AddProcess(2001, 2000, 2001, "C:\\App\\child2.exe");

  EXPECT_FALSE(HasPolicy(2000));
  EXPECT_FALSE(HasPolicy(2001));

  PolicyRule singleRule{.Action = PolicyRule::ByPassRoute{}, .Scope = PolicyScope::SingleProcess};

  registry.AddPathRule("C:\\App\\app2.exe", singleRule);
  tracker.ApplyPathRule("C:\\App\\app2.exe", singleRule);

  auto action2000 = tracker.GetAction(p2000.ProcessId);
  ASSERT_TRUE(action2000.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::ByPassRoute>(action2000.value()));

  EXPECT_FALSE(HasPolicy(2001));
}
