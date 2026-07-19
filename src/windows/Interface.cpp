#include "Interface.hpp"

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio.hpp>

#include "Asio.hpp"
#include "ConnectionTracker.hpp"
#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "ExternalQueue.hpp"
#include "Fiber.hpp"
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
  auto StartVpn(int32_t mtu, std::span<uint8_t> encryption_key) -> std::error_code override;
  auto StopEngine() -> std::error_code override;
  auto StopVpn() -> std::error_code override;

  auto AddEndpoint(const std::array<uint8_t, 16>& psk, const std::string& address) -> VpnEndpoint override;
  void RemoveEndpoint(VpnEndpoint endpoint) override;

  // Policy Interface
  void ClearPathRegistry() override;
  void AddPathPolicy(const std::string& path, const PolicyRule& policy) override;
  void RemovePathPolicy(const std::string& path) override;
  void AddPidPolicy(uint32_t pid, const PolicyRule& policy) override;
  void SetDefaultPolicy(const PolicyRule& policy) override;
  void LaunchWithPolicy(const std::string& command_line, const PolicyRule& policy) override;
  auto GetFlows() -> std::vector<FlowInfo> override;
  auto GetProcessTree() -> std::vector<ProcessInfo> override;

private:
  using BridgeTask = std::function<Omni::Fiber::Coroutine<void>(const std::shared_ptr<gh::policy::PolicyEngine>&,
                                                                const std::unique_ptr<gh::TunnelDataPlane>&)>;

  DataPlaneCallbacks& _Callbacks;
  boost::asio::io_context _IoContext;
  std::thread _AsioThread;
  Omni::Fiber::ExternalQueue<BridgeTask> _TaskQueue;
  std::atomic<bool> _Stop = false;
};

PlatformImpl::PlatformImpl(DataPlaneCallbacks& callbacks)
    : _Callbacks(callbacks), _TaskQueue(_IoContext.get_executor()) {}
PlatformImpl::~PlatformImpl() {}

auto PlatformImpl::StartEngine() -> std::error_code {
  _Stop.store(false);
  _IoContext.restart();

  _AsioThread = std::thread([this]() -> void {
    auto ioExecutor = _IoContext.get_executor();
    Omni::Fiber::AsioExecutor executor(ioExecutor);
    Omni::Fiber::Manager manager(executor);

    manager.SpawnRoot("bridge_task_processor", [this, ioExecutor]() -> Omni::Fiber::Coroutine<void> {
      auto guard = boost::asio::make_work_guard(ioExecutor);
      auto policyEngine = std::make_shared<gh::policy::PolicyEngine>(ioExecutor);
      auto dataPlane = std::make_unique<gh::TunnelDataPlane>(ioExecutor, policyEngine->GetPolicySelector(), _Callbacks);
      policyEngine->GetPolicySelector().SetInjector(dataPlane->GetInjector());

      while (!_Stop.load()) {
        co_await _TaskQueue;
        while (!_TaskQueue.IsEmpty()) {
          auto task = _TaskQueue.PopFront();
          if (task) {
            co_await task(policyEngine, dataPlane);
          }
        }
      }
      guard.reset();
      co_return;
    });

    _IoContext.run();
  });

  std::promise<ErrorCode> promise;
  auto future = promise.get_future();

  _TaskQueue.Push(
      [this, &promise](const auto& policyEngine, const auto& /*dataPlane*/) -> Omni::Fiber::Coroutine<void> {
        auto err = co_await policyEngine->Start();
        if (err) {
          promise.set_value(err);
          _Stop.store(true);
          co_return;
        }

        promise.set_value(ErrorCode{});
        co_return;
      });

  auto err = future.get();
  if (err) {
    if (_AsioThread.joinable()) {
      _AsioThread.join();
    }
    return err;
  }

  return ErrorCode{};
}

auto PlatformImpl::StartVpn(int32_t mtu, std::span<uint8_t> encryption_key) -> std::error_code {
  std::promise<ErrorCode> promise;
  auto future = promise.get_future();
  std::vector<char> key(encryption_key.begin(), encryption_key.end());

  _TaskQueue.Push([&promise, mtu, key = std::move(key)](const auto& /*policyEngine*/,
                                                        const auto& dataPlane) -> Omni::Fiber::Coroutine<void> {
    auto err = co_await dataPlane->Start(mtu, key);
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

  _TaskQueue.Push([&promise](const auto& policyEngine, const auto& dataPlane) -> Omni::Fiber::Coroutine<void> {
    policyEngine->GetPolicySelector().ClearInjector();
    auto err = co_await dataPlane->Stop();
    if (err) {
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

  _TaskQueue.Push(
      [this, &promise](const auto& policyEngine, const auto& /*dataPlane*/) -> Omni::Fiber::Coroutine<void> {
        auto err = co_await policyEngine->Stop();
        if (err) {
          promise.set_value(err);
          co_return;
        }

        promise.set_value(ErrorCode{});
        _Stop.store(true);
        co_return;
      });

  auto err = future.get();
  if (err) {
    return err;
  }

  if (_AsioThread.joinable()) {
    _AsioThread.join();
  }

  return ErrorCode{};
}

auto PlatformImpl::AddEndpoint(const std::array<uint8_t, 16>& psk, const std::string& address) -> VpnEndpoint {
  std::promise<std::shared_ptr<gh::VpnClientMultiChannelSession>> promise;
  auto future = promise.get_future();

  _TaskQueue.Push(
      [&promise, psk, address](const auto& /*policyEngine*/, const auto& dataPlane) -> Omni::Fiber::Coroutine<void> {
        auto session = co_await dataPlane->AddEndpoint(psk, address);
        promise.set_value(session);
        co_return;
      });

  return VpnEndpoint{future.get()};
}

void PlatformImpl::RemoveEndpoint(VpnEndpoint endpoint) {
  std::promise<void> promise;
  auto future = promise.get_future();
  auto session = endpoint.lock();

  _TaskQueue.Push(
      [&promise, session](const auto& /*policyEngine*/, const auto& dataPlane) -> Omni::Fiber::Coroutine<void> {
        if (session) {
          co_await dataPlane->RemoveEndpoint(session);
        }
        promise.set_value();
        co_return;
      });

  future.get();
}

void PlatformImpl::ClearPathRegistry() {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise](const auto& policyEngine, const auto& /*dataPlane*/) -> Omni::Fiber::Coroutine<void> {
    policyEngine->ClearPathRegistry();
    promise.set_value();
    co_return;
  });
  future.get();
}

void PlatformImpl::AddPathPolicy(const std::string& path, const PolicyRule& policy) {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push(
      [&promise, path, policy](const auto& policyEngine, const auto& /*dataPlane*/) -> Omni::Fiber::Coroutine<void> {
        policyEngine->AddPathPolicy(path, policy);
        promise.set_value();
        co_return;
      });
  future.get();
}

void PlatformImpl::RemovePathPolicy(const std::string& path) {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push(
      [&promise, path](const auto& policyEngine, const auto& /*dataPlane*/) -> Omni::Fiber::Coroutine<void> {
        policyEngine->RemovePathPolicy(path);
        promise.set_value();
        co_return;
      });
  future.get();
}

void PlatformImpl::AddPidPolicy(uint32_t pid, const PolicyRule& policy) {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push(
      [&promise, pid, policy](const auto& policyEngine, const auto& /*dataPlane*/) -> Omni::Fiber::Coroutine<void> {
        policyEngine->AddPidPolicy(pid, policy);
        promise.set_value();
        co_return;
      });
  future.get();
}

void PlatformImpl::SetDefaultPolicy(const PolicyRule& policy) {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push(
      [&promise, policy](const auto& policyEngine, const auto& /*dataPlane*/) -> Omni::Fiber::Coroutine<void> {
        policyEngine->SetDefaultPolicy(policy);
        promise.set_value();
        co_return;
      });
  future.get();
}

void PlatformImpl::LaunchWithPolicy(const std::string& command_line, const PolicyRule& policy) {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise, command_line, policy](const auto& policyEngine,
                                                   const auto& /*dataPlane*/) -> Omni::Fiber::Coroutine<void> {
    auto pid = policyEngine->LaunchWithPolicy(command_line, policy);
    if (pid == 0) {
      promise.set_exception(std::make_exception_ptr(std::runtime_error("LaunchWithPolicy failed")));
    } else {
      promise.set_value();
    }
    co_return;
  });
  future.get();
}

auto PlatformImpl::GetFlows() -> std::vector<FlowInfo> {
  std::promise<std::vector<FlowInfo>> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise](const auto& policyEngine, const auto& /*dataPlane*/) -> Omni::Fiber::Coroutine<void> {
    auto trackedFlows = policyEngine->GetPolicySelector().GetFlowTracker().GetFlows();
    std::vector<FlowInfo> flows;
    flows.reserve(trackedFlows.size());
    for (const auto& flow : trackedFlows) {
      FlowInfo info;
      info.ProcessId = flow.ProcessId;
      std::visit(
          [&info](const auto& key) -> void {
            using T = std::decay_t<decltype(key)>;
            if constexpr (std::is_same_v<T, ConnectionTracker::Ip4TcpKey>) {
              info.Protocol = "TCPv4";
              info.LocalAddress = key.LocalAddress.to_string();
              info.LocalPort = key.LocalPort;
              info.RemoteAddress = key.RemoteAddress.to_string();
              info.RemotePort = key.RemotePort;
            } else if constexpr (std::is_same_v<T, ConnectionTracker::Ip6TcpKey>) {
              info.Protocol = "TCPv6";
              info.LocalAddress = key.LocalAddress.to_string();
              info.LocalPort = key.LocalPort;
              info.RemoteAddress = key.RemoteAddress.to_string();
              info.RemotePort = key.RemotePort;
            } else if constexpr (std::is_same_v<T, ConnectionTracker::Ip4UdpKey>) {
              info.Protocol = "UDPv4";
              info.LocalAddress = key.LocalAddress.to_string();
              info.LocalPort = key.LocalPort;
              info.RemoteAddress = key.RemoteAddress.to_string();
              info.RemotePort = key.RemotePort;
            } else if constexpr (std::is_same_v<T, ConnectionTracker::Ip6UdpKey>) {
              info.Protocol = "UDPv6";
              info.LocalAddress = key.LocalAddress.to_string();
              info.LocalPort = key.LocalPort;
              info.RemoteAddress = key.RemoteAddress.to_string();
              info.RemotePort = key.RemotePort;
            } else if constexpr (std::is_same_v<T, ConnectionTracker::IcmpKey>) {
              info.Protocol = "ICMPv4";
              info.LocalAddress = key.LocalAddress.to_string();
              info.RemoteAddress = key.RemoteAddress.to_string();
              info.LocalPort = key.Id;
              info.RemotePort = 0;
            } else if constexpr (std::is_same_v<T, ConnectionTracker::Icmp6Key>) {
              info.Protocol = "ICMPv6";
              info.LocalAddress = key.LocalAddress.to_string();
              info.RemoteAddress = key.RemoteAddress.to_string();
              info.LocalPort = key.Id;
              info.RemotePort = 0;
            }
          },
          flow.Key);
      flows.push_back(std::move(info));
    }
    promise.set_value(std::move(flows));
    co_return;
  });
  return future.get();
}

auto PlatformImpl::GetProcessTree() -> std::vector<ProcessInfo> {
  if (_Stop.load()) {
    return {};
  }
  std::promise<std::vector<ProcessInfo>> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise](const auto& policyEngine, const auto& /*dataPlane*/) -> Omni::Fiber::Coroutine<void> {
    auto processes = policyEngine->GetPolicySelector().GetProcessTreeTracker().GetProcessTree();
    promise.set_value(std::move(processes));
    co_return;
  });
  return future.get();
}

auto CreatePlatform(DataPlaneCallbacks& callbacks) -> std::shared_ptr<PlatformInterface> {
  return std::make_shared<PlatformImpl>(callbacks);
}

} // namespace gh::Interface
