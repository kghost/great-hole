#pragma once

#include <boost/asio.hpp>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>

#include <evntrace.h>
#include <windows.h>

#include <krabs.hpp>

#include "ExternalQueue.hpp"
#include "InterfaceWin32.hpp"
#include "Logger.hpp"
#include "MoveOnlyFunction.hpp"
#include "PolicyRegistry.hpp"
#include "ServiceBase.hpp"

class TestProcessTreeTracker;

namespace gh::policy {

struct ProcessNode {
  Interface::ProcessSequence ProcessSequence{0};
  std::optional<Interface::ProcessSequence> ParentProcessSequence{0};
  Interface::ProcessId ProcessId{0};
  std::optional<std::string> ExecutablePath;
  std::optional<PolicyRule> Policy;
  std::set<Interface::ProcessSequence> Children;
};

class ProcessTreeTracker : public ServiceBase {
public:
  explicit ProcessTreeTracker(boost::asio::any_io_executor executor, const PolicyRegistry& registry);
  ~ProcessTreeTracker() override = default;

  ProcessTreeTracker(const ProcessTreeTracker&) = delete;
  auto operator=(const ProcessTreeTracker&) -> ProcessTreeTracker& = delete;
  ProcessTreeTracker(ProcessTreeTracker&&) = delete;
  auto operator=(ProcessTreeTracker&&) -> ProcessTreeTracker&& = delete;

  auto GetName() const -> std::string override { return "ProcessTreeTracker"; }

  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

  auto RegisterProcessPolicy(Interface::ProcessSequence process, const PolicyRule& rule)
      -> std::expected<void, std::string>;
  auto LaunchWithPolicy(const std::string& imagePath, const std::optional<std::string>& commandLine,
                        const PolicyRule& policy) -> std::expected<Interface::ProcessSequence, std::string>;
  void ApplyPathRule(const std::string& imagePath, const PolicyRule& rule);

  auto AddProcess(Interface::ProcessSequence process, Interface::ProcessSequence parentSeq, Interface::ProcessId pid,
                  std::optional<std::string> path) -> const ProcessNode&;
  void RemoveProcess(Interface::ProcessSequence process);
  void ClearAllMock();
  auto GetAction(Interface::ProcessId process) -> std::optional<PolicyRule::RoutingAction>;
  [[nodiscard]] auto GetProcessTree() const -> std::vector<Interface::ProcessInfo>;

private:
  friend class ::TestProcessTreeTracker;

  void EvaluatePolicyLocked(ProcessNode& node);
  void ApplyPolicyToDescendantsLocked(const std::set<Interface::ProcessSequence>& children, const PolicyRule& rule);
  void BuildInitialSnapshot();
  void EtwThreadProc();
  void HandleEtwEvent(PEVENT_RECORD record);

  static constexpr const GUID _ProcessEventsGuid = {
      .Data1 = 0x22fb2cd6, .Data2 = 0x0e7b, .Data3 = 0x422b, .Data4 = {0xa0, 0xc7, 0x2f, 0xad, 0x1f, 0xd0, 0xe7, 0x16}};

  using Task = Omni::Fiber::move_only_function<Omni::Fiber::Coroutine<void>()>;

  boost::asio::any_io_executor _Executor;
  Omni::Fiber::ExternalQueue<Task> _TaskQueue;
  const PolicyRegistry& _Registry;
  Interface::ProcessSequence _CurrentProcess;

  std::map<Interface::ProcessSequence, ProcessNode> _ProcessMap;
  std::map<Interface::ProcessId, std::reference_wrapper<ProcessNode>> _ProcessIdMap;

  TRACEHANDLE _EtwSessionHandle = 0;
  std::thread _EtwThread;
  krabs::schema_locator _SchemaLocator;
  std::string _SessionName;
  gh::base::ComponentLogger _Logger{boost::log::keywords::channel = "ProcessTreeTracker"};
};

} // namespace gh::policy
