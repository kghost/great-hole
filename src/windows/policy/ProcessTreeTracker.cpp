#include "ProcessTreeTracker.hpp"

#include <array>
#include <boost/log/trivial.hpp>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <evntcons.h>
#include <evntrace.h>
#include <tlhelp32.h>
#include <windows.h>

#include "PolicyRegistry.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"
#include "Strings.hpp"

namespace gh::policy {

namespace {

constexpr size_t kEtwPropertiesBufferExtra = 1024;

void StopOrphanedSessions() {
  constexpr unsigned kMaxSessionNameLen = 1024;
  constexpr unsigned kMaxLogfilePathLen = 1024;
  constexpr unsigned kPropertiesSize =
      sizeof(EVENT_TRACE_PROPERTIES) + (kMaxSessionNameLen * sizeof(WCHAR)) + (kMaxLogfilePathLen * sizeof(WCHAR));

  constexpr ULONG kDefaultSessionCount = 64;
  ULONG sessionCount = kDefaultSessionCount;
  std::vector<EVENT_TRACE_PROPERTIES*> sessions;
  std::vector<BYTE> buffer;
  ULONG status = ERROR_MORE_DATA;

  while (status == ERROR_MORE_DATA) {
    sessions.resize(sessionCount);
    buffer.resize(static_cast<size_t>(kPropertiesSize) * static_cast<size_t>(sessionCount));

    for (size_t i = 0; i != sessions.size(); i += 1) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      sessions[i] = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(&buffer[i * kPropertiesSize]);
      sessions[i]->Wnode.BufferSize = kPropertiesSize;
      sessions[i]->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
      sessions[i]->LogFileNameOffset = sizeof(EVENT_TRACE_PROPERTIES) + (kMaxSessionNameLen * sizeof(WCHAR));
    }

    status = QueryAllTracesW(sessions.data(), sessionCount, &sessionCount);
  }

  if (status == ERROR_SUCCESS) {
    for (ULONG i = 0; i < sessionCount; i++) {
      if (sessions[i]->LoggerNameOffset != 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-pro-bounds-pointer-arithmetic)
        const auto* loggerName = reinterpret_cast<const wchar_t*>(reinterpret_cast<const char*>(sessions[i]) +
                                                                  sessions[i]->LoggerNameOffset);
        std::wstring name(loggerName);
        if (name.starts_with(L"DesktopHoleProcessTrace_")) {
          BOOST_LOG_TRIVIAL(info) << "ProcessTreeTracker: Stopping orphaned session: "
                                  << gh::ToString(name).value_or("");
          ControlTraceW(0, loggerName, sessions[i], EVENT_TRACE_CONTROL_STOP);
        }
      }
    }
  }
}

} // namespace

ProcessTreeTracker::ProcessTreeTracker(boost::asio::any_io_executor executor,
                                       ProcessTreeTrackerDeferredCallback& callback, PolicyRegistry& registry)
    : _Executor(std::move(executor)), _TaskQueue(_Executor), _Callback(callback), _Registry(registry) {}

ProcessTreeTracker::~ProcessTreeTracker() { assert(!_Running); }

auto ProcessTreeTracker::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> {
  if (_Running) {
    co_return ErrorCode{};
  }

  StopOrphanedSessions();

  _SessionName = std::format("DesktopHoleProcessTrace_{}", GetCurrentProcessId());

  // Set up properties for StartTraceW
  std::vector<char> propertiesBuf(sizeof(EVENT_TRACE_PROPERTIES) + kEtwPropertiesBufferExtra);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertiesBuf.data());
  properties->Wnode.BufferSize = static_cast<ULONG>(propertiesBuf.size());
  properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
  properties->Wnode.ClientContext = 1;
  properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;

  // Stop any existing session first to ensure we clean it up
  ControlTraceW(0, ToWstring(_SessionName).value().c_str(), properties, EVENT_TRACE_CONTROL_STOP);

  ULONG status = StartTraceW(&_EtwSessionHandle, ToWstring(_SessionName).value().c_str(), properties);
  if (status != ERROR_SUCCESS) {
    BOOST_LOG_TRIVIAL(error) << "ProcessTreeTracker: StartTraceW failed: " << status;
    co_return gh::SysError(status);
  }

  status = EnableTraceEx2(_EtwSessionHandle, &_ProcessEventsGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                          TRACE_LEVEL_INFORMATION, 0, 0, 0, nullptr);
  if (status != ERROR_SUCCESS) {
    BOOST_LOG_TRIVIAL(error) << "ProcessTreeTracker: EnableTraceEx2 failed: " << status;
    ControlTraceW(_EtwSessionHandle, ToWstring(_SessionName).value().c_str(), properties, EVENT_TRACE_CONTROL_STOP);
    _EtwSessionHandle = 0;
    co_return gh::SysError(status);
  }

  BuildInitialSnapshot();

  _Running = true;
  _EtwThread = std::thread([this]() -> void { EtwThreadProc(); });
  co_return ErrorCode{};
}

auto ProcessTreeTracker::DoWork() -> Omni::Fiber::Coroutine<void> {
  while (true) {
    auto [cancel, hasTask] = co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(), [] -> void {}),
        Omni::Fiber::SelectPair(_TaskQueue, [] -> void {}));
    if (cancel) {
      co_return;
    }
    if (hasTask) {
      while (!_TaskQueue.IsEmpty()) {
        co_await _TaskQueue.PopFront()();
      }
    }
  }
}

auto ProcessTreeTracker::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> {
  _Running = false;

  if (_EtwSessionHandle != 0) {
    std::vector<char> propertiesBuf(sizeof(EVENT_TRACE_PROPERTIES) + kEtwPropertiesBufferExtra);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertiesBuf.data());
    properties->Wnode.BufferSize = static_cast<ULONG>(propertiesBuf.size());
    ControlTraceW(_EtwSessionHandle, ToWstring(_SessionName).value().c_str(), properties, EVENT_TRACE_CONTROL_STOP);
    _EtwSessionHandle = 0;
  }

  if (_EtwThread.joinable()) {
    _EtwThread.join();
  }

  _ProcessMap.clear();
  _PendingProcessMarks.clear();
  co_return ErrorCode{};
}

void ProcessTreeTracker::ApplyPolicyToDescendantsLocked(const std::set<DWORD>& children, const PolicyRule& rule) {
  for (DWORD childPid : children) {
    auto childIt = _ProcessMap.find(childPid);
    if (childIt != _ProcessMap.end()) {
      BOOST_LOG_TRIVIAL(info) << "PID[" << childPid << ":" << childIt->second.ExecutablePath << "] Applying policy "
                              << PolicyRuleToString(rule);
      childIt->second.Policy = rule;
      ApplyPolicyToDescendantsLocked(childIt->second.Children, rule);
    }
  }
}

void ProcessTreeTracker::EvaluatePolicyLocked(ProcessNode& node) {
  bool inherited = false;
  if (node.ParentProcessId != 0) {
    auto parentIt = _ProcessMap.find(node.ParentProcessId);
    if (parentIt != _ProcessMap.end() && parentIt->second.Policy.has_value() &&
        parentIt->second.Policy.value().Scope == PolicyScope::ProcessSubtree) {
      BOOST_LOG_TRIVIAL(info) << "PID[" << node.ProcessId << ":" << node.ExecutablePath
                              << "] Inheriting policy from parent "
                              << PolicyRuleToString(parentIt->second.Policy.value());
      node.Policy = parentIt->second.Policy.value();
      ApplyPolicyToDescendantsLocked(node.Children, parentIt->second.Policy.value());
      inherited = true;
    }
  }

  if (!inherited) {
    auto rule = _Registry.GetRuleForPath(node.ExecutablePath);
    if (rule.has_value()) {
      BOOST_LOG_TRIVIAL(info) << "PID[" << node.ProcessId << ":" << node.ExecutablePath << "] Applying policy "
                              << PolicyRuleToString(rule.value());
      node.Policy = rule;
      if (rule.value().Scope == PolicyScope::ProcessSubtree) {
        ApplyPolicyToDescendantsLocked(node.Children, rule.value());
      }
    }
  }
}

void ProcessTreeTracker::TestReEvaluatePolicy(DWORD pid) {
  auto iterator = _ProcessMap.find(pid);
  if (iterator != _ProcessMap.end()) {
    EvaluatePolicyLocked(iterator->second);
  }
}

auto ProcessTreeTracker::GetProcessPath(DWORD pid) -> std::string {
  HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (hProcess == nullptr) {
    hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
  }
  if (hProcess != nullptr) {
    std::array<wchar_t, MAX_PATH * 2> path{0};
    auto size = static_cast<DWORD>(path.size());
    if (QueryFullProcessImageNameW(hProcess, 0, path.data(), &size) != 0) {
      CloseHandle(hProcess);
      return ToString(path.data()).value_or("");
    }
    CloseHandle(hProcess);
  }
  return "";
}

auto ProcessTreeTracker::RegisterPidPolicy(DWORD pid, const PolicyRule& rule) -> bool {
  auto iterator = _ProcessMap.find(pid);
  if (iterator == _ProcessMap.end()) {
    return false;
  }

  BOOST_LOG_TRIVIAL(info) << "PID[" << pid << ":" << iterator->second.ExecutablePath << "] Register PID policy "
                          << PolicyRuleToString(rule);
  iterator->second.Policy = rule;
  if (rule.Scope == PolicyScope::ProcessSubtree) {
    ApplyPolicyToDescendantsLocked(iterator->second.Children, rule);
  }
  return true;
}

auto ProcessTreeTracker::AddProcess(DWORD pid, DWORD parentPid, const std::string& path) -> const ProcessNode& {
  auto [iterator, inserted] =
      _ProcessMap.try_emplace(pid, ProcessNode{.ProcessId = pid, .ParentProcessId = parentPid, .ExecutablePath = path});
  if (inserted) {
    if (parentPid != 0) {
      auto newParentIt = _ProcessMap.find(parentPid);
      if (newParentIt != _ProcessMap.end()) {
        newParentIt->second.Children.insert(pid);
      }
    }
    EvaluatePolicyLocked(iterator->second);
  }
  return iterator->second;
}

void ProcessTreeTracker::RemoveProcess(DWORD pid) {
  auto iterator = _ProcessMap.find(pid);
  if (iterator != _ProcessMap.end()) {
    for (DWORD childPid : iterator->second.Children) {
      auto childIt = _ProcessMap.find(childPid);
      if (childIt != _ProcessMap.end()) {
        childIt->second.ParentProcessId = 0;
      }
    }
    if (iterator->second.ParentProcessId != 0) {
      auto parentIt = _ProcessMap.find(iterator->second.ParentProcessId);
      if (parentIt != _ProcessMap.end()) {
        parentIt->second.Children.erase(pid);
      }
    }
    _ProcessMap.erase(iterator);
  }
}

void ProcessTreeTracker::ClearAllMock() {
  _ProcessMap.clear();
  _PendingProcessMarks.clear();
}

void ProcessTreeTracker::AddPendingMark(DWORD pid, const std::shared_ptr<VpnClientMultiChannel::Mark>& mark) {
  _PendingProcessMarks.try_emplace(pid, mark);
}

auto ProcessTreeTracker::GetAction(DWORD pid) const -> std::optional<PolicyRule::RoutingAction> {
  if (pid == GetCurrentProcessId()) {
    return PolicyRegistry::GetCurrentProcessAction();
  }
  auto iterator = _ProcessMap.find(pid);
  if (iterator != _ProcessMap.end()) {
    const auto& policy = iterator->second.Policy;
    if (policy.has_value()) {
      return policy.value().Action;
    } else {
      return _Registry.GetDefaultAction();
    }
  }
  return std::nullopt;
}

auto ProcessTreeTracker::GetProcessTree() const -> std::vector<Interface::ProcessInfo> {
  std::vector<Interface::ProcessInfo> list;
  list.reserve(_ProcessMap.size());
  for (const auto& [pid, node] : _ProcessMap) {
    list.push_back(Interface::ProcessInfo{
        .ProcessId = node.ProcessId,
        .ParentProcessId = node.ParentProcessId,
        .Policy = node.Policy,
    });
  }
  return list;
}

auto ProcessTreeTracker::GetPendingProcesses() const -> std::vector<Interface::PendingProcessInfo> {
  std::vector<Interface::PendingProcessInfo> pending;
  pending.reserve(_PendingProcessMarks.size());
  for (const auto& [pid, mark] : _PendingProcessMarks) {
    pending.push_back({.ProcessId = pid, .QueueSize = mark->GetPendingQueueSize()});
  }
  return pending;
}

void ProcessTreeTracker::BuildInitialSnapshot() {
  HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (hSnapshot == INVALID_HANDLE_VALUE) {
    return;
  }

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL result = Process32FirstW(hSnapshot, &entry); result != 0; result = Process32NextW(hSnapshot, &entry)) {
    _ProcessMap[entry.th32ProcessID] = ProcessNode{
        .ProcessId = entry.th32ProcessID,
        .ParentProcessId = entry.th32ParentProcessID,
        .ExecutablePath = GetProcessPath(entry.th32ProcessID),
    };
  }
  for (auto& [pid, node] : _ProcessMap) {
    if (node.ParentProcessId != 0) {
      auto parentIt = _ProcessMap.find(node.ParentProcessId);
      if (parentIt != _ProcessMap.end()) {
        parentIt->second.Children.insert(pid);
      }
    }
  }
  CloseHandle(hSnapshot);

  std::vector<DWORD> rootPids;
  for (const auto& [pid, node] : _ProcessMap) {
    if (node.ParentProcessId == 0 || !_ProcessMap.contains(node.ParentProcessId)) {
      rootPids.push_back(pid);
    }
  }

  auto evaluateTreeTopDown = [this](this auto&& self, DWORD pid) -> void {
    auto iterator = _ProcessMap.find(pid);
    if (iterator == _ProcessMap.end()) {
      return;
    }
    auto& node = iterator->second;
    auto rule = _Registry.GetRuleForPath(node.ExecutablePath);
    if (rule.has_value()) {
      BOOST_LOG_TRIVIAL(info) << "PID[" << pid << ":" << node.ExecutablePath << "] Initial policy "
                              << PolicyRuleToString(rule.value());
      node.Policy = rule;
      if (rule.value().Scope == PolicyScope::ProcessSubtree) {
        ApplyPolicyToDescendantsLocked(node.Children, rule.value());
      } else {
        for (DWORD childPid : node.Children) {
          self(childPid);
        }
      }
    } else {
      for (DWORD childPid : node.Children) {
        self(childPid);
      }
    }
  };

  for (DWORD pid : rootPids) {
    evaluateTreeTopDown(pid);
  }
}

void ProcessTreeTracker::EtwThreadProc() {
  EVENT_TRACE_LOGFILEW traceLogfile{};
  auto loggerName = ToWstring(_SessionName).value();
  traceLogfile.LoggerName = loggerName.data();
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
  traceLogfile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
  traceLogfile.EventRecordCallback = [](PEVENT_RECORD record) -> void {
    auto* tracker = static_cast<ProcessTreeTracker*>(record->UserContext);
    if (tracker) {
      tracker->HandleEtwEvent(record);
    }
  };
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
  traceLogfile.Context = this;

  TRACEHANDLE traceHandle = OpenTraceW(&traceLogfile);
  if (traceHandle == INVALID_PROCESSTRACE_HANDLE) {
    BOOST_LOG_TRIVIAL(error) << "ProcessTreeTracker: OpenTraceW failed: " << GetLastError();
    std::vector<char> propertiesBuf(sizeof(EVENT_TRACE_PROPERTIES) + kEtwPropertiesBufferExtra);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertiesBuf.data());
    properties->Wnode.BufferSize = static_cast<ULONG>(propertiesBuf.size());
    ControlTraceW(_EtwSessionHandle, ToWstring(_SessionName).value().c_str(), properties, EVENT_TRACE_CONTROL_STOP);
    _EtwSessionHandle = 0;
    _Running = false;
    return;
  }

  ULONG status = ProcessTrace(&traceHandle, 1, nullptr, nullptr);
  if (status != ERROR_SUCCESS && status != ERROR_CANCELLED) {
    BOOST_LOG_TRIVIAL(error) << "ProcessTreeTracker: ProcessTrace failed: " << status;
  }

  CloseTrace(traceHandle);
}

void ProcessTreeTracker::HandleEtwEvent(PEVENT_RECORD record) {
  if (!_Running) {
    return;
  }

  if (record->EventHeader.ProviderId != _ProcessEventsGuid) {
    return;
  }

  struct ProcessStartEvent {
    uint32_t ProcessId{};
    FILETIME CreateTime{};
    uint32_t ParentProcessId{};
    uint32_t SessionId{};
    WCHAR ImageName[0]; // NOLINT(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
  };

  struct ProcessStopEvent {
    uint32_t ProcessId{};
    FILETIME CreateTime{};
    FILETIME ExitTime{};
    uint32_t ExitCode{};
    uint32_t TokenElevationType{};
    uint32_t HandleCount{};
    uint64_t CommitCharge{};
    uint64_t CommitPeak{};
    char ImageName[0]; // NOLINT(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
  };

  auto eventId = record->EventHeader.EventDescriptor.Id;
  if (eventId == 1) {
    constexpr size_t ProcessStartEventHeaderSize = sizeof(ProcessStartEvent);
    if (record->UserDataLength >= ProcessStartEventHeaderSize) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      const auto* data = reinterpret_cast<ProcessStartEvent*>(record->UserData);
      uint32_t pid = data->ProcessId;
      uint32_t parentPid = data->ParentProcessId;
      auto execPath =
          // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
          ToString(std::wstring_view(data->ImageName,
                                     (record->UserDataLength - ProcessStartEventHeaderSize) / sizeof(WCHAR)))
              .value_or("");
      _TaskQueue.Push([this, pid, parentPid, execPath]() -> Omni::Fiber::Coroutine<void> {
        const auto& node = AddProcess(pid, parentPid, execPath);
        if (auto pending = _PendingProcessMarks.find(pid); pending != _PendingProcessMarks.end()) {
          const auto policy = node.Policy;
          auto action = policy.has_value() ? policy.value().Action : _Registry.GetDefaultAction();
          co_await _Callback.ProcessTreeTrackerContinue(pending->second, action);
        }
      });
    }
  } else if (eventId == 2) {
    if (record->UserDataLength >= 4) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      auto* data = reinterpret_cast<ProcessStopEvent*>(record->UserData);
      uint32_t pid = data->ProcessId;
      _TaskQueue.Push([this, pid]() -> Omni::Fiber::Coroutine<void> {
        RemoveProcess(pid);
        co_return;
      });
    }
  }
}

} // namespace gh::policy
