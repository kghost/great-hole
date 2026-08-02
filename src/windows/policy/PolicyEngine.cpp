#include "PolicyEngine.hpp"

#include <boost/asio.hpp>
#include <memory>
#include <utility>

#include "Interface.hpp"
#include "AutoHandle.hpp"
#include "Process.hpp"
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

void PolicyEngine::ClearPathRegistry() { _Registry.Clear(); }

void PolicyEngine::AddPathPolicy(const std::string& path, const PolicyRule& policy) {
  _Registry.AddPathRule(path, policy);
}

void PolicyEngine::RemovePathPolicy(const std::string& path) { _Registry.RemovePathRule(path); }

auto PolicyEngine::AddProcessPolicy(Interface::ProcessSequence process, const PolicyRule& policy)
    -> std::expected<void, std::string> {
  return _Selector.GetProcessTreeTracker().RegisterProcessPolicy(process, policy);
}

void PolicyEngine::SetDefaultPolicy(const PolicyRule& policy) { _Registry.SetDefaultAction(policy.Action); }

auto PolicyEngine::LaunchWithPolicy(const std::string& commandLine, const PolicyRule& policy)
    -> std::expected<Interface::ProcessSequence, std::string> {
  STARTUPINFOW startupInfo{};
  startupInfo.cb = sizeof(startupInfo);
  PROCESS_INFORMATION processInfo{};

  auto cmdLineCopy = gh::ToWstring(commandLine);
  if (!cmdLineCopy.has_value()) {
    return std::unexpected("Failed to convert command line to wstring");
  }

  if (CreateProcessW(nullptr, cmdLineCopy.value().data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr,
                     &startupInfo, &processInfo) == FALSE) {
    return std::unexpected("Failed to create process");
  }

  AutoHandle hThread(processInfo.hThread);
  AutoHandle hProcess(processInfo.hProcess);

  auto process = GetProcessSequence(hProcess.Get());
  if (!process.has_value()) {
    BOOST_LOG_TRIVIAL(error) << "PolicyEngine::LaunchWithPolicy: Failed to get process sequence number for PID "
                             << processInfo.dwProcessId;
    if (TerminateProcess(hProcess.Get(), -1) == FALSE) {
      BOOST_LOG_TRIVIAL(error) << "PolicyEngine::LaunchWithPolicy: TerminateProcess failed for PID "
                               << processInfo.dwProcessId;
    }
    return std::unexpected("Failed to get process sequence number");
  }

  auto parent = GetProcessSequence(GetCurrentProcessId());
  assert(parent.has_value());
  const auto& node = _Selector.GetProcessTreeTracker().AddProcess(process.value(), parent.value(),
                                                                  processInfo.dwProcessId, commandLine);
  auto res = _Selector.GetProcessTreeTracker().RegisterProcessPolicy(node.ProcessSequence, policy);
  if (!res) {
    BOOST_LOG_TRIVIAL(error) << "PolicyEngine::LaunchWithPolicy: Failed to register process policy: " << res.error();
    if (TerminateProcess(hProcess.Get(), -1) == FALSE) {
      BOOST_LOG_TRIVIAL(error) << "PolicyEngine::LaunchWithPolicy: TerminateProcess failed for PID "
                               << processInfo.dwProcessId;
    }
    return std::unexpected("Failed to register process policy");
  }

  ResumeThread(hThread.Get());

  return node.ProcessSequence;
}

} // namespace gh::policy
