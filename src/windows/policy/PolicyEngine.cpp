#include "PolicyEngine.hpp"

#include <boost/asio.hpp>
#include <memory>
#include <utility>

#include "Strings.hpp"

namespace gh::policy {

PolicyEngine::PolicyEngine(boost::asio::any_io_executor executor)
    : _Executor(std::move(executor)), _Selector(_Executor, _Registry),
      _Sniffer(std::make_shared<gh::WinDivertFlowSniffer>(_Executor, _Selector.GetFlowTracker())) {}

PolicyEngine::~PolicyEngine() {
  assert(_State != State::kRunning && "PolicyEngine must be stopped before destruction");
}

auto PolicyEngine::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> {
  auto err1 = co_await _Selector.GetProcessTreeTracker().Start();
  if (err1) {
    co_return err1;
  }

  auto err2 = co_await _Sniffer->Start();
  if (err2) {
    co_await _Selector.GetProcessTreeTracker().Stop();
    co_return err2;
  }

  co_return ErrorCode{};
}

auto PolicyEngine::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> {
  auto err1 = co_await _Sniffer->Stop();
  auto err2 = co_await _Selector.GetProcessTreeTracker().Stop();

  if (err1) {
    co_return err1;
  }
  if (err2) {
    co_return err2;
  }

  co_return ErrorCode{};
}

void PolicyEngine::ClearRegistry() { _Registry.Clear(); }

void PolicyEngine::AddPathPolicy(const std::string& path, const PolicyRule& policy) {
  _Registry.AddPathRule(path, policy);
}

void PolicyEngine::RemovePathPolicy(const std::string& path) { _Registry.RemovePathRule(path); }

void PolicyEngine::AddPidPolicy(DWORD pid, const PolicyRule& policy) {
  _Selector.GetProcessTreeTracker().RegisterPidPolicy(pid, policy);
}

void PolicyEngine::SetDefaultPolicy(const PolicyRule& policy) { _Registry.SetDefaultAction(policy.Action); }

auto PolicyEngine::LaunchWithPolicy(const std::string& commandLine, const PolicyRule& policy) -> uint32_t {
  STARTUPINFOW startupInfo{};
  startupInfo.cb = sizeof(startupInfo);
  PROCESS_INFORMATION processInfo{};

  auto cmdLineCopy = gh::ToWstring(commandLine);
  if (!cmdLineCopy.has_value()) {
    return 0;
  }

  if (CreateProcessW(nullptr, cmdLineCopy.value().data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr,
                     &startupInfo, &processInfo) == FALSE) {
    BOOST_LOG_TRIVIAL(error) << "PolicyEngine::LaunchWithPolicy: CreateProcessW failed: " << GetLastError();
    return 0;
  }

  _Selector.GetProcessTreeTracker().AddProcess(processInfo.dwProcessId, GetCurrentProcessId(), commandLine);
  _Selector.GetProcessTreeTracker().RegisterPidPolicy(processInfo.dwProcessId, policy);

  ResumeThread(processInfo.hThread);

  CloseHandle(processInfo.hThread);
  CloseHandle(processInfo.hProcess);

  return processInfo.dwProcessId;
}

} // namespace gh::policy
