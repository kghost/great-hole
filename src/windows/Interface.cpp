#include "Interface.hpp"

#include <boost/log/trivial.hpp>
#include <future>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio.hpp>

#include "Asio.hpp"
#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "ExternalQueue.hpp"
#include "Fiber.hpp"
#include "InterfaceMonitor.hpp"
#include "Logger.hpp"
#include "Manager.hpp"
#include "PolicyEngine.hpp"
#include "TunnelDataPlane.hpp"
#include "VpnClientMultiChannel.hpp"

namespace gh::Interface {

class PlatformImpl : public PlatformInterface {
public:
  explicit PlatformImpl(DataPlaneCallbacks& callbacks);
  ~PlatformImpl() override;

  PlatformImpl(const PlatformImpl&) = delete;
  auto operator=(const PlatformImpl&) -> PlatformImpl& = delete;
  PlatformImpl(PlatformImpl&&) = delete;
  auto operator=(PlatformImpl&&) -> PlatformImpl& = delete;

  auto StartEngine() -> std::error_code override;
  auto StartVpn(std::span<IpAddress> addresses, int32_t mtu, std::span<uint8_t> encryption_key)
      -> std::error_code override;
  auto StopEngine() -> std::error_code override;
  auto StopVpn() -> std::error_code override;

  auto AddEndpoint(const PskType& psk, const std::string& address) -> VpnEndpoint override;
  void RemoveEndpoint(VpnEndpoint endpoint) override;

  void StartEndpoint(VpnEndpoint endpoint) override;
  void StopEndpoint(VpnEndpoint endpoint) override;
  auto GetTrafficStats(VpnEndpoint endpoint) -> std::optional<VpnTrafficStats> override;

  // Policy Interface
  void ClearPathRegistry() override;
  void AddPathPolicy(const std::string& path, const PolicyRule& policy) override;
  void RemovePathPolicy(const std::string& path) override;
  auto AddProcessPolicy(ProcessSequence process, const PolicyRule& policy) -> std::expected<void, std::string> override;
  void SetDefaultPolicy(const PolicyRule& policy) override;
  auto LaunchWithPolicy(const std::string& imagePath, const std::optional<std::string>& commandLine,
                        const PolicyRule& policy) -> std::expected<ProcessSequence, std::string> override;
  auto GetFlows() -> std::vector<FlowInfo> override;
  auto GetConnections() -> std::vector<TrackedConnectionInfo> override;
  auto GetProcessTree() -> std::vector<ProcessInfo> override;

  // Logging Interface
  void SetLogLevel(LogLevel level) override;
  void SetProcessTreeTrackerLogLevel(LogLevel level) override;
  void SetFlowTrackerLogLevel(LogLevel level) override;
  void SetPolicySelectorLogLevel(LogLevel level) override;

private:
  void StartThread();
  void JoinThread();

  struct PlatformContext {
    std::shared_ptr<gh::policy::PolicyEngine> PolicyEngine;
    std::shared_ptr<gh::windows::network::InterfaceMonitor> InterfaceMonitor;
    std::unique_ptr<gh::TunnelDataPlane> DataPlane;
    gh::base::LogConfiguration LogConfig;
    bool Running = true;
  };

  using BridgeTask = std::function<Omni::Fiber::Coroutine<void>(PlatformContext&)>;

  DataPlaneCallbacks& _Callbacks;
  boost::asio::io_context _IoContext;
  std::thread _AsioThread;
  Omni::Fiber::ExternalQueue<BridgeTask> _TaskQueue;
};

PlatformImpl::PlatformImpl(DataPlaneCallbacks& callbacks)
    : _Callbacks(callbacks), _TaskQueue(_IoContext.get_executor()) {
  StartThread();
}

PlatformImpl::~PlatformImpl() { JoinThread(); }

void PlatformImpl::StartThread() {
  if (_AsioThread.joinable()) {
    return;
  }
  _IoContext.restart();

  _AsioThread = std::thread([this]() -> void {
    auto ioExecutor = _IoContext.get_executor();
    Omni::Fiber::AsioExecutor executor(ioExecutor);
    Omni::Fiber::Manager manager(executor);

    manager.SpawnRoot("bridge_task_processor", [this, ioExecutor]() -> Omni::Fiber::Coroutine<void> {
      auto guard = boost::asio::make_work_guard(ioExecutor);
      PlatformContext context;
      while (context.Running) {
        co_await _TaskQueue;
        while (!_TaskQueue.IsEmpty()) {
          auto task = _TaskQueue.PopFront();
          if (task) {
            co_await task(context);
          }
        }
      }
      guard.reset();
      co_return;
    });

    _IoContext.run();
  });
}

void PlatformImpl::JoinThread() {
  _TaskQueue.Push([](auto& context) -> Omni::Fiber::Coroutine<void> {
    context.Running = false;
    co_return;
  });
  _AsioThread.join();
}

auto PlatformImpl::StartEngine() -> std::error_code {
  std::promise<ErrorCode> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([this, &promise](auto& context) -> Omni::Fiber::Coroutine<void> {
    assert(!context.PolicyEngine);
    assert(!context.InterfaceMonitor);
    context.PolicyEngine = std::make_shared<gh::policy::PolicyEngine>(_IoContext.get_executor());
    context.InterfaceMonitor = std::make_shared<gh::windows::network::InterfaceMonitor>(_IoContext.get_executor());
    auto err = co_await context.PolicyEngine->Start();
    if (err) {
      context.PolicyEngine.reset();
      context.InterfaceMonitor.reset();
      promise.set_value(err);
      co_return;
    }
    err = co_await context.InterfaceMonitor->Start();
    if (err) {
      co_await context.PolicyEngine->Stop();
      context.PolicyEngine.reset();
      context.InterfaceMonitor.reset();
      promise.set_value(err);
      co_return;
    }
    promise.set_value(ErrorCode{});
    co_return;
  });
  return future.get();
}

auto PlatformImpl::StopEngine() -> std::error_code {
  std::promise<ErrorCode> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise](auto& context) -> Omni::Fiber::Coroutine<void> {
    auto err = co_await context.InterfaceMonitor->Stop();
    if (err) {
      BOOST_LOG_TRIVIAL(error) << "Failed to stop interface monitor: " << err.message();
    }
    context.InterfaceMonitor.reset();
    err = co_await context.PolicyEngine->Stop();
    if (err) {
      BOOST_LOG_TRIVIAL(error) << "Failed to stop policy engine: " << err.message();
    }
    context.PolicyEngine.reset();
    promise.set_value(ErrorCode{});
    co_return;
  });
  return future.get();
}

auto PlatformImpl::StartVpn(std::span<IpAddress> addresses, int32_t mtu, std::span<uint8_t> encryption_key)
    -> std::error_code {
  std::promise<ErrorCode> promise;
  auto future = promise.get_future();
  std::vector<char> key(encryption_key.begin(), encryption_key.end());
  _TaskQueue.Push(
      [this, &promise, key = std::move(key), addresses, mtu](auto& context) -> Omni::Fiber::Coroutine<void> {
        context.DataPlane =
            std::make_unique<gh::TunnelDataPlane>(_IoContext.get_executor(), context.PolicyEngine->GetPolicySelector(),
                                                  _Callbacks, addresses, mtu, *context.InterfaceMonitor);
        auto err = co_await context.DataPlane->Start(key);
        if (err) {
          promise.set_value(err);
          co_return;
        }
        promise.set_value(ErrorCode{});
        co_return;
      });
  return future.get();
}

auto PlatformImpl::StopVpn() -> std::error_code {
  std::promise<ErrorCode> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise](auto& context) -> Omni::Fiber::Coroutine<void> {
    auto err = co_await context.DataPlane->Stop();
    context.DataPlane.reset();
    if (err) {
      promise.set_value(err);
      co_return;
    }
    promise.set_value(ErrorCode{});
    co_return;
  });
  return future.get();
}

auto PlatformImpl::AddEndpoint(const PskType& psk, const std::string& address) -> VpnEndpoint {
  std::promise<std::weak_ptr<gh::VpnClientMultiChannelSession>> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise, psk, address](auto& context) -> Omni::Fiber::Coroutine<void> {
    promise.set_value(context.DataPlane->AddEndpoint(psk, address));
    co_return;
  });
  return VpnEndpoint{future.get()};
}

void PlatformImpl::RemoveEndpoint(VpnEndpoint endpoint) {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise, endpoint](auto& context) -> Omni::Fiber::Coroutine<void> {
    context.DataPlane->RemoveEndpoint(endpoint);
    promise.set_value();
    co_return;
  });
  future.get();
}

void PlatformImpl::StartEndpoint(VpnEndpoint endpoint) {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise, endpoint](auto& context) -> Omni::Fiber::Coroutine<void> {
    co_await context.DataPlane->StartEndpoint(endpoint);
    promise.set_value();
    co_return;
  });
  future.get();
}

void PlatformImpl::StopEndpoint(VpnEndpoint endpoint) {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise, endpoint](auto& context) -> Omni::Fiber::Coroutine<void> {
    co_await context.DataPlane->StopEndpoint(endpoint);
    promise.set_value();
    co_return;
  });
  future.get();
}

auto PlatformImpl::GetTrafficStats(VpnEndpoint endpoint) -> std::optional<VpnTrafficStats> {
  std::promise<std::optional<VpnTrafficStats>> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise, endpoint](auto& context) -> Omni::Fiber::Coroutine<void> {
    promise.set_value(context.DataPlane->GetTrafficStats(endpoint));
    co_return;
  });
  return future.get();
}

void PlatformImpl::ClearPathRegistry() {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise](auto& context) -> Omni::Fiber::Coroutine<void> {
    context.PolicyEngine->ClearPathRegistry();
    promise.set_value();
    co_return;
  });
  future.get();
}

void PlatformImpl::AddPathPolicy(const std::string& path, const PolicyRule& policy) {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise, path, policy](auto& context) -> Omni::Fiber::Coroutine<void> {
    context.PolicyEngine->AddPathPolicy(path, policy);
    promise.set_value();
    co_return;
  });
  future.get();
}

void PlatformImpl::RemovePathPolicy(const std::string& path) {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise, path](auto& context) -> Omni::Fiber::Coroutine<void> {
    context.PolicyEngine->RemovePathPolicy(path);
    promise.set_value();
    co_return;
  });
  future.get();
}

auto PlatformImpl::AddProcessPolicy(ProcessSequence process, const PolicyRule& policy)
    -> std::expected<void, std::string> {
  std::promise<std::expected<void, std::string>> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise, process, policy](auto& context) -> Omni::Fiber::Coroutine<void> {
    promise.set_value(context.PolicyEngine->AddProcessPolicy(process, policy));
    co_return;
  });
  return future.get();
}

void PlatformImpl::SetDefaultPolicy(const PolicyRule& policy) {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise, policy](auto& context) -> Omni::Fiber::Coroutine<void> {
    context.PolicyEngine->SetDefaultPolicy(policy);
    promise.set_value();
    co_return;
  });
  future.get();
}

auto PlatformImpl::LaunchWithPolicy(const std::string& imagePath, const std::optional<std::string>& commandLine,
                                    const PolicyRule& policy) -> std::expected<ProcessSequence, std::string> {
  std::promise<std::expected<ProcessSequence, std::string>> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise, imagePath, commandLine, policy](auto& context) -> Omni::Fiber::Coroutine<void> {
    promise.set_value(context.PolicyEngine->LaunchWithPolicy(imagePath, commandLine, policy));
    co_return;
  });
  return future.get();
}

auto PlatformImpl::GetFlows() -> std::vector<FlowInfo> {
  std::promise<std::vector<FlowInfo>> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise](auto& context) -> Omni::Fiber::Coroutine<void> {
    auto trackedFlows = context.PolicyEngine->GetPolicySelector().GetFlowTracker().GetFlows();
    promise.set_value(std::move(trackedFlows));
    co_return;
  });
  return future.get();
}

auto PlatformImpl::GetConnections() -> std::vector<TrackedConnectionInfo> {
  std::promise<std::vector<TrackedConnectionInfo>> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise](auto& context) -> Omni::Fiber::Coroutine<void> {
    auto connections = context.DataPlane->GetConnections();
    promise.set_value(std::move(connections));
    co_return;
  });
  return future.get();
}

auto PlatformImpl::GetProcessTree() -> std::vector<ProcessInfo> {
  std::promise<std::vector<ProcessInfo>> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise](auto& context) -> Omni::Fiber::Coroutine<void> {
    auto processes = context.PolicyEngine->GetPolicySelector().GetProcessTreeTracker().GetProcessTree();
    promise.set_value(std::move(processes));
    co_return;
  });
  return future.get();
}

void PlatformImpl::SetLogLevel(LogLevel level) {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise, level](auto& context) -> Omni::Fiber::Coroutine<void> {
    context.LogConfig.SetLogLevel(level);
    promise.set_value();
    co_return;
  });
  future.get();
}

void PlatformImpl::SetProcessTreeTrackerLogLevel(LogLevel level) {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise, level](auto& context) -> Omni::Fiber::Coroutine<void> {
    context.LogConfig.SetComponentLogLevel("ProcessTreeTracker", level);
    promise.set_value();
    co_return;
  });
  future.get();
}

void PlatformImpl::SetPolicySelectorLogLevel(LogLevel level) {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise, level](auto& context) -> Omni::Fiber::Coroutine<void> {
    context.LogConfig.SetComponentLogLevel("PolicySelector", level);
    promise.set_value();
    co_return;
  });
  future.get();
}

void PlatformImpl::SetFlowTrackerLogLevel(LogLevel level) {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise, level](auto& context) -> Omni::Fiber::Coroutine<void> {
    context.LogConfig.SetComponentLogLevel("FlowTracker", level);
    promise.set_value();
    co_return;
  });
  future.get();
}

auto CreatePlatform(DataPlaneCallbacks& callbacks) -> std::shared_ptr<PlatformInterface> {
  return std::make_shared<PlatformImpl>(callbacks);
}

} // namespace gh::Interface
