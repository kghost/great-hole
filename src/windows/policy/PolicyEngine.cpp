#include "PolicyEngine.hpp"

#include <boost/asio.hpp>
#include <memory>
#include <utility>

#include "Interface.hpp"

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

auto PolicyEngine::LaunchWithPolicy(const std::string& imagePath, const std::optional<std::string>& commandLine,
                                    const PolicyRule& policy)
    -> std::expected<Interface::ProcessSequence, std::string> {
  return _Selector.GetProcessTreeTracker().LaunchWithPolicy(imagePath, commandLine, policy);
}

} // namespace gh::policy
