#pragma once

#include <boost/asio.hpp>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>

#include <evntrace.h>
#include <windows.h>

#include "ExternalQueue.hpp"
#include "Logger.hpp"
#include "MoveOnlyFunction.hpp"
#include "PolicyRegistry.hpp"
#include "ServiceBase.hpp"

namespace gh::policy {

struct ProcessNode {
  DWORD ProcessId;
  DWORD ParentProcessId;
  std::string ExecutablePath;
  std::optional<PolicyRule> Policy;
  std::set<DWORD> Children;
};

class ProcessTreeTracker : public ServiceBase {
public:
  explicit ProcessTreeTracker(boost::asio::any_io_executor executor, PolicyRegistry& registry);
  ~ProcessTreeTracker() override = default;

  ProcessTreeTracker(const ProcessTreeTracker&) = delete;
  auto operator=(const ProcessTreeTracker&) -> ProcessTreeTracker& = delete;
  ProcessTreeTracker(ProcessTreeTracker&&) = delete;
  auto operator=(ProcessTreeTracker&&) -> ProcessTreeTracker&& = delete;

  auto GetName() const -> std::string override { return "ProcessTreeTracker"; }

  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

  auto RegisterPidPolicy(DWORD pid, const PolicyRule& rule) -> bool;

  auto AddProcess(DWORD pid, DWORD parentPid, const std::string& path) -> const ProcessNode&;
  void RemoveProcess(DWORD pid);
  void ClearAllMock();
  auto GetAction(DWORD pid) -> std::optional<PolicyRule::RoutingAction>;
  [[nodiscard]] auto GetProcessTree() const -> std::vector<Interface::ProcessInfo>;
  void TestReEvaluatePolicy(DWORD pid);

private:
  [[nodiscard]] static auto GetProcessPath(DWORD pid) -> std::string;
  void EvaluatePolicyLocked(ProcessNode& node);
  void ApplyPolicyToDescendantsLocked(const std::set<DWORD>& children, const PolicyRule& rule);
  void BuildInitialSnapshot();
  void EtwThreadProc();
  void HandleEtwEvent(PEVENT_RECORD record);

  static constexpr const GUID _ProcessEventsGuid = {
      .Data1 = 0x22fb2cd6, .Data2 = 0x0e7b, .Data3 = 0x422b, .Data4 = {0xa0, 0xc7, 0x2f, 0xad, 0x1f, 0xd0, 0xe7, 0x16}};

  using Task = Omni::Fiber::move_only_function<Omni::Fiber::Coroutine<void>()>;

  boost::asio::any_io_executor _Executor;
  Omni::Fiber::ExternalQueue<Task> _TaskQueue;
  PolicyRegistry& _Registry;

  std::map<DWORD, ProcessNode> _ProcessMap;

  TRACEHANDLE _EtwSessionHandle = 0;
  std::thread _EtwThread;
  std::string _SessionName;
  gh::base::ComponentLogger _Logger{boost::log::keywords::channel = "ProcessTreeTracker"};
};

} // namespace gh::policy
