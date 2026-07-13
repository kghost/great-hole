#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>

#include <evntrace.h>
#include <windows.h>

#include "EventQueue.hpp"
#include "PolicyRegistry.hpp"
#include "ServiceBase.hpp"

namespace gh::policy {

class ProcessTreeTrackerDeferredCallback {
public:
  explicit ProcessTreeTrackerDeferredCallback() = default;
  virtual ~ProcessTreeTrackerDeferredCallback() = default;

  ProcessTreeTrackerDeferredCallback(const ProcessTreeTrackerDeferredCallback&) = delete;
  auto operator=(const ProcessTreeTrackerDeferredCallback&) -> ProcessTreeTrackerDeferredCallback& = delete;
  ProcessTreeTrackerDeferredCallback(ProcessTreeTrackerDeferredCallback&&) = delete;
  auto operator=(ProcessTreeTrackerDeferredCallback&&) -> ProcessTreeTrackerDeferredCallback& = delete;

  virtual auto ProcessTreeTrackerContinue(const std::shared_ptr<VpnClientMultiChannel::Mark>& mark,
                                          const PolicyRule::RoutingAction& action) -> Omni::Fiber::Coroutine<void> = 0;
};

struct ProcessNode {
  DWORD ProcessId;
  DWORD ParentProcessId;
  std::string ExecutablePath;
  std::optional<PolicyRule> Policy;
  std::set<DWORD> Children;
};

class ProcessTreeTracker : public ServiceBase {
public:
  explicit ProcessTreeTracker(boost::asio::any_io_executor executor, ProcessTreeTrackerDeferredCallback& callback,
                              PolicyRegistry& registry);
  ~ProcessTreeTracker() override;

  ProcessTreeTracker(const ProcessTreeTracker&) = delete;
  auto operator=(const ProcessTreeTracker&) -> ProcessTreeTracker& = delete;
  ProcessTreeTracker(ProcessTreeTracker&&) = delete;
  auto operator=(ProcessTreeTracker&&) -> ProcessTreeTracker& = delete;

  auto GetName() const -> std::string override { return "ProcessTreeTracker"; }

  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

  auto RegisterPidPolicy(DWORD pid, const PolicyRule& rule) -> bool;

  auto AddProcess(DWORD pid, DWORD parentPid, const std::string& path) -> const ProcessNode&;
  void RemoveProcess(DWORD pid);
  void ClearAllMock();
  [[nodiscard]] auto GetPolicy(DWORD pid) const -> std::optional<PolicyRule>;
  void TestReEvaluatePolicy(DWORD pid);

  void AddPendingMark(DWORD pid, const std::shared_ptr<VpnClientMultiChannel::Mark>& mark);

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
  Omni::Fiber::EventQueue<Task> _TaskQueue;
  ProcessTreeTrackerDeferredCallback& _Callback;
  PolicyRegistry& _Registry;

  std::map<DWORD, ProcessNode> _ProcessMap;
  // TODO: change to weak_ptr
  std::map<DWORD, std::shared_ptr<VpnClientMultiChannel::Mark>> _PendingProcessMarks;

  TRACEHANDLE _EtwSessionHandle = 0;
  std::thread _EtwThread;
  std::atomic<bool> _Running{false};
  std::string _SessionName;
};

} // namespace gh::policy
