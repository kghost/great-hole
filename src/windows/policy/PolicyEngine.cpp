#include "PolicyEngine.hpp"

#include <boost/asio.hpp>
#include <memory>
#include <utility>

#include <winsafer.h>

#include "AutoHandle.hpp"
#include "Interface.hpp"
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

auto PolicyEngine::LaunchWithPolicy(const std::string& imagePath, const std::string& commandLine,
                                    const PolicyRule& policy)
    -> std::expected<Interface::ProcessSequence, std::string> {
  STARTUPINFOW startupInfo{};
  startupInfo.cb = sizeof(startupInfo);
  PROCESS_INFORMATION processInfo{};

  std::optional<std::wstring> imagePathW;
  if (!imagePath.empty()) {
    imagePathW = gh::ToWstring(imagePath);
    if (!imagePathW.has_value()) {
      return std::unexpected("Failed to convert image path to wstring");
    }
  }

  std::optional<std::wstring> commandLineW;
  if (!commandLine.empty()) {
    commandLineW = gh::ToWstring(commandLine);
    if (!commandLineW.has_value()) {
      return std::unexpected("Failed to convert command line to wstring");
    }
  }

  LPCWSTR appName = imagePathW.has_value() ? imagePathW.value().c_str() : nullptr;
  LPWSTR cmdLine = commandLineW.has_value() ? commandLineW.value().data() : nullptr;

  AutoHandle hPrimaryToken;

  HWND hShellWnd = GetShellWindow();
  if (hShellWnd == nullptr) {
    return std::unexpected("Failed to get shell window");
  }

  DWORD explorerPid = 0;
  GetWindowThreadProcessId(hShellWnd, &explorerPid);
  if (explorerPid == 0) {
    return std::unexpected("Failed to get explorer process ID");
  }

  AutoHandle hExplorerProcess(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, explorerPid));
  if (!hExplorerProcess.IsValid()) {
    auto err = SysError(GetLastError());
    return std::unexpected(std::format("Failed to open explorer process: {}", err.message()));
  }

  AutoHandle hExplorerToken;
  if (OpenProcessToken(hExplorerProcess.Get(), TOKEN_DUPLICATE | TOKEN_QUERY, hExplorerToken.Put()) != FALSE) {
    DuplicateTokenEx(hExplorerToken.Get(), MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary,
                     hPrimaryToken.Put());
  }

  if (!hPrimaryToken.IsValid()) {
    return std::unexpected("Failed to obtain non-privileged primary user token");
  }

  if (CreateProcessWithTokenW(hPrimaryToken.Get(), 0, appName, cmdLine, CREATE_SUSPENDED, nullptr, nullptr,
                              &startupInfo, &processInfo) == FALSE) {
    auto err = SysError(GetLastError());
    return std::unexpected(std::format("Failed to create process: {}", err.message()));
  }

  AutoHandle hThread(processInfo.hThread);
  AutoHandle hProcess(processInfo.hProcess);

  auto process = GetProcessSequence(hProcess.Get());
  if (!process.has_value()) {
    if (TerminateProcess(hProcess.Get(), -1) == FALSE) {
      BOOST_LOG_TRIVIAL(error) << "PolicyEngine::LaunchWithPolicy: TerminateProcess failed for PID "
                               << processInfo.dwProcessId;
    }
    return std::unexpected("Failed to get process sequence number");
  }

  auto parent = GetProcessSequence(GetCurrentProcessId());
  assert(parent.has_value());
  const std::string& pathOrCmd = !imagePath.empty() ? imagePath : commandLine;
  const auto& node =
      _Selector.GetProcessTreeTracker().AddProcess(process.value(), parent.value(), processInfo.dwProcessId, pathOrCmd);
  auto res = _Selector.GetProcessTreeTracker().RegisterProcessPolicy(node.ProcessSequence, policy);
  if (!res) {
    if (TerminateProcess(hProcess.Get(), -1) == FALSE) {
      BOOST_LOG_TRIVIAL(error) << "PolicyEngine::LaunchWithPolicy: TerminateProcess failed for PID "
                               << processInfo.dwProcessId;
    }
    return std::unexpected(std::format("Failed to register process policy {}", res.error()));
  }

  ResumeThread(hThread.Get());

  return node.ProcessSequence;
}

} // namespace gh::policy
