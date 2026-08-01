#include "ProcessTreeTracker.hpp"

#include <array>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/sources/severity_feature.hpp>
#include <boost/log/trivial.hpp>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <evntcons.h>
#include <evntrace.h>
#include <psapi.h>
#include <windows.h>

#include "Interface.hpp"
#include "PolicyRegistry.hpp"
#include "Process.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"
#include "Strings.hpp"
#include "Utils/Nothing.hpp"
#include "Utils/Span.hpp"

namespace gh::policy {

namespace {

constexpr size_t kEtwPropertiesBufferExtra = 1024;

void StopOrphanedSessions(gh::base::ComponentLogger& logger) {
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
          BOOST_LOG_SEV(logger, boost::log::trivial::info)
              << "ProcessTreeTracker: Stopping orphaned session: " << gh::ToString(name).value_or("");
          ControlTraceW(0, loggerName, sessions[i], EVENT_TRACE_CONTROL_STOP);
        }
      }
    }
  }
}

} // namespace

ProcessTreeTracker::ProcessTreeTracker(boost::asio::any_io_executor executor, PolicyRegistry& registry)
    : _Executor(std::move(executor)), _TaskQueue(_Executor), _Registry(registry) {
  WithProcessHandle(GetCurrentProcessId(), [this](std::optional<HANDLE> handle) -> Nothing {
    assert(handle.has_value());
    auto seq = GetProcessSequence(handle.value());
    assert(seq.has_value());
    _CurrentProcess = seq.value();
    return {};
  });
}

auto ProcessTreeTracker::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> {
  StopOrphanedSessions(_Logger);
  _SessionName = std::format("DesktopHoleProcessTrace_{}", GetCurrentProcessId());

  std::vector<char> propertiesBuf(sizeof(EVENT_TRACE_PROPERTIES) + kEtwPropertiesBufferExtra);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertiesBuf.data());
  properties->Wnode.BufferSize = static_cast<ULONG>(propertiesBuf.size());
  properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
  properties->Wnode.ClientContext = 1;
  properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;

  ControlTraceW(0, ToWstring(_SessionName).value().c_str(), properties, EVENT_TRACE_CONTROL_STOP);

  ULONG status = StartTraceW(&_EtwSessionHandle, ToWstring(_SessionName).value().c_str(), properties);
  if (status != ERROR_SUCCESS) {
    BOOST_LOG_SEV(_Logger, boost::log::trivial::error) << "ProcessTreeTracker: StartTraceW failed: " << status;
    co_return gh::SysError(status);
  }

  status = EnableTraceEx2(_EtwSessionHandle, &_ProcessEventsGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                          TRACE_LEVEL_INFORMATION, 0, 0, 0, nullptr);
  if (status != ERROR_SUCCESS) {
    BOOST_LOG_SEV(_Logger, boost::log::trivial::error) << "ProcessTreeTracker: EnableTraceEx2 failed: " << status;
    ControlTraceW(_EtwSessionHandle, ToWstring(_SessionName).value().c_str(), properties, EVENT_TRACE_CONTROL_STOP);
    _EtwSessionHandle = 0;
    co_return gh::SysError(status);
  }

  BuildInitialSnapshot();

  auto result = RegisterProcessPolicy(
      _CurrentProcess, PolicyRule{.Action = PolicyRule::ByPassRoute{}, .Scope = PolicyScope::ProcessSubtree});
  assert(result.has_value());

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

  _TaskQueue.Clear();
  _ProcessMap.clear();
  co_return ErrorCode{};
}

void ProcessTreeTracker::ApplyPolicyToDescendantsLocked(const std::set<Interface::ProcessSequence>& children,
                                                        const PolicyRule& rule) {
  for (auto child : children) {
    auto childIt = _ProcessMap.find(child);
    if (childIt != _ProcessMap.end()) {
      BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
          << "child[" << child << "]:" << childIt->second.ExecutablePath.value_or("") << "] Applying policy "
          << PolicyRuleToString(rule);
      if (childIt->second.ProcessSequence != _CurrentProcess) {
        childIt->second.Policy = rule;
        ApplyPolicyToDescendantsLocked(childIt->second.Children, rule);
      }
    }
  }
}

void ProcessTreeTracker::EvaluatePolicyLocked(ProcessNode& node) {
  if (node.ProcessSequence == _CurrentProcess) {
    return;
  }

  if (node.ParentProcessSequence.has_value()) {
    auto parentIt = _ProcessMap.find(node.ParentProcessSequence.value());
    if (parentIt != _ProcessMap.end() && parentIt->second.Policy.has_value() &&
        parentIt->second.Policy.value().Scope == PolicyScope::ProcessSubtree) {
      BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
          << "Process[" << node.ProcessSequence << ":" << node.ExecutablePath.value_or("")
          << "] Inheriting policy from parent Process[" << parentIt->first << "] "
          << PolicyRuleToString(parentIt->second.Policy.value());
      node.Policy = parentIt->second.Policy.value();
      ApplyPolicyToDescendantsLocked(node.Children, parentIt->second.Policy.value());
      return;
    }
  }

  if (node.ExecutablePath.has_value()) {
    auto rule = _Registry.GetRuleForPath(node.ExecutablePath.value());
    if (rule.has_value()) {
      BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
          << "Process[" << node.ProcessSequence << ":" << node.ExecutablePath.value_or("") << "] Applying policy "
          << PolicyRuleToString(rule.value());
      node.Policy = rule;
      if (rule.value().Scope == PolicyScope::ProcessSubtree) {
        ApplyPolicyToDescendantsLocked(node.Children, rule.value());
        return;
      }
    }
  }
}

auto ProcessTreeTracker::RegisterProcessPolicy(Interface::ProcessSequence process, const PolicyRule& rule)
    -> std::expected<void, std::string> {
  if (process == _CurrentProcess &&
      (!std::holds_alternative<PolicyRule::ByPassRoute>(rule.Action) || rule.Scope != PolicyScope::ProcessSubtree)) {
    return std::unexpected("Cannot register policy for VPN process");
  }

  auto iterator = _ProcessMap.find(process);
  if (iterator == _ProcessMap.end()) {
    return std::unexpected("Process not found");
  }

  BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
      << "Seq[" << process << ":PID " << iterator->second.ProcessId << ":"
      << iterator->second.ExecutablePath.value_or("") << "] Register policy " << PolicyRuleToString(rule);
  iterator->second.Policy = rule;
  if (rule.Scope == PolicyScope::ProcessSubtree) {
    ApplyPolicyToDescendantsLocked(iterator->second.Children, rule);
  }
  return {};
}

auto ProcessTreeTracker::AddProcess(Interface::ProcessSequence process, Interface::ProcessSequence parent,
                                    Interface::ProcessId pid, std::optional<std::string> path) -> const ProcessNode& {
  auto [iterator, inserted] = _ProcessMap.try_emplace(process, ProcessNode{.ProcessSequence = process,
                                                                           .ParentProcessSequence = parent,
                                                                           .ProcessId = pid,
                                                                           .ExecutablePath = std::move(path)});
  if (inserted) {
    _ProcessIdMap.insert_or_assign(pid, std::ref(iterator->second));
    auto newParentIt = _ProcessMap.find(parent);
    if (newParentIt != _ProcessMap.end()) {
      newParentIt->second.Children.insert(process);
    }
    EvaluatePolicyLocked(iterator->second);
  }
  return iterator->second;
}

void ProcessTreeTracker::RemoveProcess(Interface::ProcessSequence process) {
  auto iterator = _ProcessMap.find(process);
  if (iterator != _ProcessMap.end()) {
    for (Interface::ProcessSequence childSeq : iterator->second.Children) {
      auto childIt = _ProcessMap.find(childSeq);
      if (childIt != _ProcessMap.end()) {
        childIt->second.ParentProcessSequence = std::nullopt;
      }
    }
    if (iterator->second.ParentProcessSequence.has_value()) {
      auto parentIt = _ProcessMap.find(iterator->second.ParentProcessSequence.value());
      if (parentIt != _ProcessMap.end()) {
        parentIt->second.Children.erase(process);
      }
    }
    auto idIt = _ProcessIdMap.find(iterator->second.ProcessId);
    if (idIt != _ProcessIdMap.end() && idIt->second.get().ProcessSequence == process) {
      _ProcessIdMap.erase(idIt);
    }
    _ProcessMap.erase(iterator);
  } else {
    BOOST_LOG_SEV(_Logger, boost::log::trivial::warning)
        << "ProcessTreeTracker: Process Seq " << process << " not found in process map";
  }
}

void ProcessTreeTracker::ClearAllMock() {
  _ProcessMap.clear();
  _ProcessIdMap.clear();
}

auto ProcessTreeTracker::GetAction(Interface::ProcessId process) -> std::optional<PolicyRule::RoutingAction> {
  auto iterator = _ProcessIdMap.find(process);
  if (iterator != _ProcessIdMap.end()) {
    return iterator->second.get().Policy.transform([](auto& policy) -> auto { return policy.Action; });
  }

  auto ensureProcessInMap = [&](this auto& self,
                                Interface::ProcessId pid) -> std::optional<std::reference_wrapper<const ProcessNode>> {
    BOOST_LOG_SEV(_Logger, boost::log::trivial::info) << "ProcessTreeTracker: Ensure process in map PID:" << pid;

    auto iterator = _ProcessIdMap.find(pid);
    if (iterator != _ProcessIdMap.end()) {
      BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
          << "ProcessTreeTracker: Process Seq:" << iterator->second.get().ProcessSequence << " PID:" << pid
          << " already in map";
      return iterator->second;
    }

    return WithProcessHandle(
        pid, [&](std::optional<HANDLE> handle) -> std::optional<std::reference_wrapper<const ProcessNode>> {
          if (!handle.has_value()) {
            BOOST_LOG_SEV(_Logger, boost::log::trivial::warning) << "ProcessTreeTracker: PID " << pid << " not found";
            return std::nullopt;
          }
          auto seq = GetProcessSequence(handle.value());
          if (!seq.has_value()) {
            BOOST_LOG_SEV(_Logger, boost::log::trivial::warning)
                << "ProcessTreeTracker: PID " << pid << " Seq not found";
            return std::nullopt;
          }
          auto parentPid = GetParentProcessId(handle.value());
          if (!parentPid.has_value()) {
            BOOST_LOG_SEV(_Logger, boost::log::trivial::warning)
                << "ProcessTreeTracker: PID " << pid << " Parent PID not found";
            return std::nullopt;
          }

          auto parentNodeOpt = self(parentPid.value());
          if (parentNodeOpt.has_value()) {
            Interface::ProcessSequence parentSeq = parentNodeOpt->get().ProcessSequence;
            BOOST_LOG_SEV(_Logger, boost::log::trivial::trace) << "ProcessTreeTracker: Add Process Seq:" << seq.value()
                                                               << " PID:" << pid << " Parent Seq:" << parentSeq;
            return AddProcess(seq.value(), parentSeq, pid, GetProcessPath(handle.value()));
          } else {
            BOOST_LOG_SEV(_Logger, boost::log::trivial::warning)
                << "ProcessTreeTracker: PID " << parentPid.value() << " not found";
            return std::nullopt;
          }
        });
  };

  auto nodeOpt = ensureProcessInMap(process);
  if (nodeOpt.has_value()) {
    return nodeOpt.value().get().Policy.transform([](auto& policy) -> auto { return policy.Action; });
  }

  return _Registry.GetDefaultAction();
}

auto ProcessTreeTracker::GetProcessTree() const -> std::vector<Interface::ProcessInfo> {
  std::vector<Interface::ProcessInfo> list;
  list.reserve(_ProcessMap.size());
  for (const auto& [seq, node] : _ProcessMap) {
    list.push_back(Interface::ProcessInfo{
        .Process = node.ProcessSequence,
        .ParentProcess = node.ParentProcessSequence,
        .ProcessId = node.ProcessId,
        .Policy = node.Policy,
    });
  }
  return list;
}

void ProcessTreeTracker::BuildInitialSnapshot() {
  std::vector<DWORD> pids(1024);
  DWORD bytesReturned = 0;
  while (true) {
    if (EnumProcesses(pids.data(), static_cast<DWORD>(pids.size() * sizeof(DWORD)), &bytesReturned) == 0) {
      return;
    }
    if (bytesReturned < pids.size() * sizeof(DWORD)) {
      break;
    }
    pids.resize(pids.size() * 2);
  }

  auto count = bytesReturned / sizeof(DWORD);

  struct TempEntry {
    Interface::ProcessId Pid;
    Interface::ProcessSequence Seq;
    std::optional<Interface::ProcessId> ParentId;
    std::optional<std::string> Path;
  };

  std::vector<TempEntry> entries;
  entries.reserve(count);

  for (auto pid : pids) {
    WithProcessHandle(pid, [&](std::optional<HANDLE> handle) -> Nothing {
      if (handle.has_value()) {
        auto seq = GetProcessSequence(handle.value());
        if (seq.has_value()) {
          entries.push_back(TempEntry{.Pid = pid,
                                      .Seq = seq.value(),
                                      .ParentId = GetParentProcessId(handle.value()),
                                      .Path = GetProcessPath(handle.value())});
        }
      }
      return {};
    });
  }

  std::map<Interface::ProcessId, Interface::ProcessSequence> pidToSeqMap;
  for (const auto& item : entries) {
    pidToSeqMap[item.Pid] = item.Seq;
  }

  for (const auto& item : entries) {
    auto [iterator, inserted] =
        _ProcessMap.try_emplace(item.Seq, ProcessNode{.ProcessSequence = item.Seq,
                                                      .ParentProcessSequence = item.ParentId.and_then(
                                                          [&](auto pid) -> std::optional<Interface::ProcessSequence> {
                                                            auto iterator = pidToSeqMap.find(pid);
                                                            if (iterator != pidToSeqMap.end()) {
                                                              return iterator->second;
                                                            }
                                                            return std::nullopt;
                                                          }),
                                                      .ProcessId = item.Pid,
                                                      .ExecutablePath = item.Path});
    if (inserted) {
      auto [iterator2, inserted2] = _ProcessIdMap.try_emplace(item.Pid, std::ref(iterator->second));
      assert(inserted2);
    }
  }

  for (auto& [process, node] : _ProcessMap) {
    if (node.ParentProcessSequence.has_value()) {
      auto parentIt = _ProcessMap.find(node.ParentProcessSequence.value());
      if (parentIt != _ProcessMap.end()) {
        parentIt->second.Children.insert(process);
      }
    }
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
    BOOST_LOG_SEV(_Logger, boost::log::trivial::error) << "ProcessTreeTracker: OpenTraceW failed: " << GetLastError();
    std::vector<char> propertiesBuf(sizeof(EVENT_TRACE_PROPERTIES) + kEtwPropertiesBufferExtra);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertiesBuf.data());
    properties->Wnode.BufferSize = static_cast<ULONG>(propertiesBuf.size());
    ControlTraceW(_EtwSessionHandle, ToWstring(_SessionName).value().c_str(), properties, EVENT_TRACE_CONTROL_STOP);
    _EtwSessionHandle = 0;
    return;
  }

  ULONG status = ProcessTrace(&traceHandle, 1, nullptr, nullptr);
  if (status != ERROR_SUCCESS && status != ERROR_CANCELLED) {
    BOOST_LOG_SEV(_Logger, boost::log::trivial::error) << "ProcessTreeTracker: ProcessTrace failed: " << status;
  }

  CloseTrace(traceHandle);
}

void ProcessTreeTracker::HandleEtwEvent(PEVENT_RECORD record) {
  if (record->EventHeader.ProviderId != _ProcessEventsGuid) {
    return;
  }

  auto eventId = record->EventHeader.EventDescriptor.Id;
  auto version = record->EventHeader.EventDescriptor.Version;

  if (eventId == 1) {
    if (version >= 3) {
      // Version 3 and 4
      // Fields:
      // 1. ProcessID (UInt32) - 4 bytes
      // 2. ProcessSequenceNumber (UInt64) - 8 bytes
      // 3. CreateTime (FILETIME) - 8 bytes
      // 4. ParentProcessID (UInt32) - 4 bytes
      // 5. ParentProcessSequenceNumber (UInt64) - 8 bytes
      // 6. SessionID (UInt32) - 4 bytes
      // 7. Flags (UInt32) - 4 bytes
      // 8. ProcessTokenElevationType (UInt32) - 4 bytes
      // 9. ProcessTokenIsElevated (UInt32) - 4 bytes
      // 10. MandatoryLabel (SID) - variable length (starts at offset 48)
      // 11. ImageName (UnicodeString) - variable length
      constexpr size_t kStartEventV3HeaderSize = 48;
      constexpr size_t kStartEventV3ParentSeqOffset = 24;
      constexpr size_t kSidHeaderSize = 8;
      constexpr size_t kSidSubAuthoritySize = 4;

      Interface::ProcessId pid = 0;
      Interface::ProcessSequence seq = 0;
      Interface::ProcessSequence parentSeq = 0;
      std::string imagePath;

      if (record->UserDataLength >= kStartEventV3HeaderSize) {
        const auto userData =
            std::span<const uint8_t>(static_cast<const uint8_t*>(record->UserData), record->UserDataLength);
        pid = SpanToField<Interface::ProcessId, 0>(userData);
        seq = SpanToField<Interface::ProcessSequence, 4>(userData);
        parentSeq = SpanToField<Interface::ProcessSequence, kStartEventV3ParentSeqOffset>(userData);

        if (record->UserDataLength >= kStartEventV3HeaderSize + 2) {
          const auto sidPtr = userData.subspan<kStartEventV3HeaderSize>();
          auto subAuthorityCount = SpanToField<uint8_t, 1>(sidPtr);
          size_t sidSize = kSidHeaderSize + (kSidSubAuthoritySize * subAuthorityCount);

          size_t imageNameOffset = kStartEventV3HeaderSize + sidSize;
          if (record->UserDataLength > imageNameOffset) {
            const auto wstrSpan =
                View<const wchar_t>(userData.subspan(imageNameOffset, record->UserDataLength - imageNameOffset));
            const auto firstNull = std::ranges::find(wstrSpan, L'\0');
            const auto wstrView = std::wstring_view(wstrSpan.data(), std::distance(wstrSpan.begin(), firstNull));
            imagePath = ToString(wstrView).value_or("");
          }
        }
      }

      BOOST_LOG_SEV(_Logger, boost::log::trivial::trace)
          << "Event log[Add]: Seq " << seq << ", PID " << pid << ", ParentSeq " << parentSeq << ", Path " << imagePath;
      _TaskQueue.Push([this, seq, parentSeq, pid, imagePath]() -> Omni::Fiber::Coroutine<void> {
        AddProcess(seq, parentSeq, pid, imagePath);
        co_return;
      });
    } else {
      BOOST_LOG_SEV(_Logger, boost::log::trivial::error)
          << "Event log[Add]: Unsupported event version " << static_cast<int>(version);
    }
  } else if (eventId == 2) {
    if (version >= 2) {
      constexpr size_t kStopEventMinSize = 12;
      if (record->UserDataLength >= kStopEventMinSize) {
        const auto userData =
            std::span<const uint8_t>(static_cast<const uint8_t*>(record->UserData), record->UserDataLength);
        auto seq = SpanToField<Interface::ProcessSequence, 4>(userData);
        BOOST_LOG_SEV(_Logger, boost::log::trivial::trace) << "Event log[Remove]: Seq " << seq;
        _TaskQueue.Push([this, seq]() -> Omni::Fiber::Coroutine<void> {
          RemoveProcess(seq);
          co_return;
        });
      }
    } else {
      BOOST_LOG_SEV(_Logger, boost::log::trivial::error)
          << "Event log[Remove]: Unsupported event version " << static_cast<int>(version);
    }
  }
}

} // namespace gh::policy
