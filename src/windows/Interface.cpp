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
#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "EventQueue.hpp"
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

  auto Start(int32_t mtu, std::span<uint8_t> encryption_key) -> std::error_code override;
  auto Stop() -> std::error_code override;

  auto AddEndpoint(const std::array<uint8_t, 16>& psk, const std::string& address) -> VpnEndpoint override;
  void RemoveEndpoint(VpnEndpoint endpoint) override;

  // Policy Interface
  void ClearRegistry() override;
  void AddPathBypassRule(const std::string& path, PolicyScope scope) override;
  void AddPathEndpointRule(const std::string& path, VpnEndpoint endpoint, PolicyScope scope) override;
  void RemovePathRule(const std::string& path) override;
  void AddPidEndpointRule(uint32_t pid, VpnEndpoint endpoint, PolicyScope scope) override;
  void SetDefaultEndpoint(VpnEndpoint endpoint) override;
  void SetDefaultBypass() override;
  void LaunchWithPolicy(const std::string& command_line, VpnEndpoint endpoint, PolicyScope scope) override;

private:
  using BridgeTask = std::function<Omni::Fiber::Coroutine<void>(const std::shared_ptr<gh::policy::PolicyEngine>&,
                                                                const std::unique_ptr<gh::TunnelDataPlane>&)>;

  DataPlaneCallbacks& _Callbacks;
  std::thread _AsioThread;
  // TODO: EventQueue is not thread-safe
  Omni::Fiber::EventQueue<BridgeTask> _TaskQueue;
  std::atomic<bool> _Stop = false;
};

PlatformImpl::PlatformImpl(DataPlaneCallbacks& callbacks) : _Callbacks(callbacks) {}
PlatformImpl::~PlatformImpl() {}

auto PlatformImpl::Start(int32_t mtu, std::span<uint8_t> encryption_key) -> std::error_code {
  _AsioThread = std::thread([this]() -> void {
    boost::asio::io_context ioContext;
    auto ioExecutor = ioContext.get_executor();
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

    ioContext.run();
  });

  std::promise<ErrorCode> promise;
  auto future = promise.get_future();
  std::vector<char> key(encryption_key.begin(), encryption_key.end());

  _TaskQueue.Push([this, &promise, mtu, key = std::move(key)](const auto& policyEngine,
                                                              const auto& dataPlane) -> Omni::Fiber::Coroutine<void> {
    auto err = co_await policyEngine->Start();
    if (err) {
      promise.set_value(err);
      _Stop.store(true);
      co_return;
    }

    err = co_await dataPlane->Start(mtu, key);
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

auto PlatformImpl::Stop() -> std::error_code {
  std::promise<ErrorCode> promise;
  auto future = promise.get_future();

  _TaskQueue.Push([this, &promise](const auto& policyEngine, const auto& dataPlane) -> Omni::Fiber::Coroutine<void> {
    auto err = co_await dataPlane->Stop();
    if (err) {
      promise.set_value(err);
      co_return;
    }

    err = co_await policyEngine->Stop();
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

void PlatformImpl::ClearRegistry() {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise](const auto& policyEngine, const auto& /*dataPlane*/) -> Omni::Fiber::Coroutine<void> {
    policyEngine->ClearRegistry();
    promise.set_value();
    co_return;
  });
  future.get();
}

void PlatformImpl::AddPathBypassRule(const std::string& path, PolicyScope scope) {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push(
      [&promise, path, scope](const auto& policyEngine, const auto& /*dataPlane*/) -> Omni::Fiber::Coroutine<void> {
        policyEngine->AddPathBypassRule(path, scope);
        promise.set_value();
        co_return;
      });
  future.get();
}

void PlatformImpl::AddPathEndpointRule(const std::string& path, VpnEndpoint endpoint, PolicyScope scope) {
  std::promise<void> promise;
  auto future = promise.get_future();
  auto session = endpoint.lock();
  _TaskQueue.Push([&promise, path, session, scope](const auto& policyEngine,
                                                   const auto& /*dataPlane*/) -> Omni::Fiber::Coroutine<void> {
    policyEngine->AddPathEndpointRule(path, session, scope);
    promise.set_value();
    co_return;
  });
  future.get();
}

void PlatformImpl::RemovePathRule(const std::string& path) {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push(
      [&promise, path](const auto& policyEngine, const auto& /*dataPlane*/) -> Omni::Fiber::Coroutine<void> {
        policyEngine->RemovePathRule(path);
        promise.set_value();
        co_return;
      });
  future.get();
}

void PlatformImpl::AddPidEndpointRule(uint32_t pid, VpnEndpoint endpoint, PolicyScope scope) {
  std::promise<void> promise;
  auto future = promise.get_future();
  auto session = endpoint.lock();
  _TaskQueue.Push([&promise, pid, session, scope](const auto& policyEngine,
                                                  const auto& /*dataPlane*/) -> Omni::Fiber::Coroutine<void> {
    policyEngine->AddPidEndpointRule(pid, session, scope);
    promise.set_value();
    co_return;
  });
  future.get();
}

void PlatformImpl::SetDefaultEndpoint(VpnEndpoint endpoint) {
  std::promise<void> promise;
  auto future = promise.get_future();
  auto session = endpoint.lock();
  _TaskQueue.Push(
      [&promise, session](const auto& policyEngine, const auto& /*dataPlane*/) -> Omni::Fiber::Coroutine<void> {
        policyEngine->SetDefaultEndpoint(session);
        promise.set_value();
        co_return;
      });
  future.get();
}

void PlatformImpl::SetDefaultBypass() {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([&promise](const auto& policyEngine, const auto& /*dataPlane*/) -> Omni::Fiber::Coroutine<void> {
    policyEngine->SetDefaultBypass();
    promise.set_value();
    co_return;
  });
  future.get();
}

void PlatformImpl::LaunchWithPolicy(const std::string& command_line, VpnEndpoint endpoint, PolicyScope scope) {
  std::promise<void> promise;
  auto future = promise.get_future();
  auto session = endpoint.lock();
  _TaskQueue.Push([&promise, command_line, session, scope](const auto& policyEngine,
                                                           const auto& /*dataPlane*/) -> Omni::Fiber::Coroutine<void> {
    auto pid = policyEngine->LaunchWithPolicy(command_line, session, scope);
    if (pid == 0) {
      promise.set_exception(std::make_exception_ptr(std::runtime_error("LaunchWithPolicy failed")));
    } else {
      promise.set_value();
    }
    co_return;
  });
  future.get();
}

auto CreatePlatform(DataPlaneCallbacks& callbacks) -> std::shared_ptr<PlatformInterface> {
  return std::make_shared<PlatformImpl>(callbacks);
}

} // namespace gh::Interface
