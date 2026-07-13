#include "Interface.hpp"

#include <algorithm>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio.hpp>

#include "Asio.hpp"
#include "Coroutine.hpp"
#include "DeferredPacketInjector.hpp"
#include "EventQueue.hpp"
#include "Fiber.hpp"
#include "Manager.hpp"
#include "PolicyEngine.hpp"
#include "TunnelDataPlane.hpp"
#include "VpnClientMultiChannel.hpp"

namespace gh::Interface {

class PlatformImpl : public PlatformInterface, public gh::DeferredPacketInjector {
public:
  explicit PlatformImpl(DataPlaneCallbacks& callbacks);
  ~PlatformImpl() override;

  PlatformImpl(const PlatformImpl&) = delete;
  auto operator=(const PlatformImpl&) -> PlatformImpl& = delete;
  PlatformImpl(PlatformImpl&&) = delete;
  auto operator=(PlatformImpl&&) -> PlatformImpl& = delete;

  void Start(int32_t mtu, std::span<uint8_t> encryption_key) override;
  void Stop() override;

  auto AddEndpoint(const std::string& psk, const std::string& address) -> VpnEndpoint override;
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

  // From gh::DeferredPacketInjector
  auto Inject(Packet&& packet) -> Omni::Fiber::Coroutine<void> override;

private:
  using BridgeTask = std::function<Omni::Fiber::Coroutine<void>()>;

  DataPlaneCallbacks& _Callbacks;
  std::shared_ptr<boost::asio::io_context> _IoContext;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> _WorkGuard;
  std::shared_ptr<gh::policy::PolicyEngine> _PolicyEngine;
  std::unique_ptr<gh::TunnelDataPlane> _DataPlane;
  std::thread _AsioThread;
  Omni::Fiber::EventQueue<BridgeTask> _TaskQueue;
};

PlatformImpl::PlatformImpl(DataPlaneCallbacks& callbacks)
    : _Callbacks(callbacks), _IoContext(std::make_shared<boost::asio::io_context>()),
      _WorkGuard(boost::asio::make_work_guard(*_IoContext)) {
  auto ioExecutor = _IoContext->get_executor();
  _PolicyEngine = std::make_shared<gh::policy::PolicyEngine>(ioExecutor, *this);
  _DataPlane = std::make_unique<gh::TunnelDataPlane>(ioExecutor, _PolicyEngine->GetPolicySelector(), _Callbacks);

  _AsioThread = std::thread([this]() {
    auto ioExecutor = _IoContext->get_executor();
    Omni::Fiber::AsioExecutor executor(ioExecutor);
    Omni::Fiber::Manager manager(executor);

    manager.SpawnRoot("bridge_task_processor", [this]() -> Omni::Fiber::Coroutine<void> {
      bool stop = false;
      while (!stop) {
        co_await _TaskQueue;
        while (!_TaskQueue.IsEmpty()) {
          auto task = _TaskQueue.PopFront();
          if (task) {
            co_await task();
          } else {
            stop = true;
          }
        }
      }
      co_return;
    });

    _IoContext->run();
  });
}

PlatformImpl::~PlatformImpl() {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([this, &promise]() -> Omni::Fiber::Coroutine<void> {
    if (_DataPlane) {
      co_await _DataPlane->Stop();
    }
    if (_PolicyEngine) {
      co_await _PolicyEngine->Stop();
    }
    promise.set_value();
    co_return;
  });
  future.wait();

  _TaskQueue.Push(BridgeTask(nullptr));
  _WorkGuard.reset();
  _IoContext->stop();

  if (_AsioThread.joinable()) {
    _AsioThread.join();
  }
}

void PlatformImpl::Start(int32_t mtu, std::span<uint8_t> encryption_key) {
  std::promise<void> promise;
  auto future = promise.get_future();
  std::vector<char> key(encryption_key.begin(), encryption_key.end());

  _TaskQueue.Push([this, &promise, mtu, key = std::move(key)]() -> Omni::Fiber::Coroutine<void> {
    try {
      auto err1 = co_await _PolicyEngine->Start();
      if (err1) {
        throw std::runtime_error(err1.message());
      }
      co_await _DataPlane->Start(mtu, key);
      promise.set_value();
    } catch (...) {
      promise.set_exception(std::current_exception());
    }
    co_return;
  });

  future.get();
}

void PlatformImpl::Stop() {
  std::promise<void> promise;
  auto future = promise.get_future();

  _TaskQueue.Push([this, &promise]() -> Omni::Fiber::Coroutine<void> {
    try {
      co_await _DataPlane->Stop();
      co_await _PolicyEngine->Stop();
      promise.set_value();
    } catch (...) {
      promise.set_exception(std::current_exception());
    }
    co_return;
  });

  future.get();
}

auto PlatformImpl::AddEndpoint(const std::string& psk, const std::string& address) -> VpnEndpoint {
  std::promise<std::shared_ptr<gh::VpnClientMultiChannelSession>> promise;
  auto future = promise.get_future();

  _TaskQueue.Push([this, &promise, psk, address]() -> Omni::Fiber::Coroutine<void> {
    try {
      gh::UdpDynMux::PskType pskArray;
      std::copy_n(psk.begin(), std::min(psk.size(), pskArray.size()), pskArray.begin());
      auto session = co_await _DataPlane->AddEndpoint(pskArray, address);
      promise.set_value(session);
    } catch (...) {
      promise.set_exception(std::current_exception());
    }
    co_return;
  });

  auto session = future.get();
  if (!session) {
    throw std::runtime_error("Failed to add endpoint");
  }
  return VpnEndpoint{session};
}

void PlatformImpl::RemoveEndpoint(VpnEndpoint endpoint) {
  std::promise<void> promise;
  auto future = promise.get_future();
  auto session = endpoint.lock();

  _TaskQueue.Push([this, &promise, session]() -> Omni::Fiber::Coroutine<void> {
    try {
      if (session) {
        co_await _DataPlane->RemoveEndpoint(session);
      }
      promise.set_value();
    } catch (...) {
      promise.set_exception(std::current_exception());
    }
    co_return;
  });

  future.get();
}

void PlatformImpl::ClearRegistry() {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([this, &promise]() -> Omni::Fiber::Coroutine<void> {
    _PolicyEngine->ClearRegistry();
    promise.set_value();
    co_return;
  });
  future.get();
}

void PlatformImpl::AddPathBypassRule(const std::string& path, PolicyScope scope) {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([this, &promise, path, scope]() -> Omni::Fiber::Coroutine<void> {
    _PolicyEngine->AddPathBypassRule(path, scope);
    promise.set_value();
    co_return;
  });
  future.get();
}

void PlatformImpl::AddPathEndpointRule(const std::string& path, VpnEndpoint endpoint, PolicyScope scope) {
  std::promise<void> promise;
  auto future = promise.get_future();
  auto session = endpoint.lock();
  _TaskQueue.Push([this, &promise, path, session, scope]() -> Omni::Fiber::Coroutine<void> {
    _PolicyEngine->AddPathEndpointRule(path, session, scope);
    promise.set_value();
    co_return;
  });
  future.get();
}

void PlatformImpl::RemovePathRule(const std::string& path) {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([this, &promise, path]() -> Omni::Fiber::Coroutine<void> {
    _PolicyEngine->RemovePathRule(path);
    promise.set_value();
    co_return;
  });
  future.get();
}

void PlatformImpl::AddPidEndpointRule(uint32_t pid, VpnEndpoint endpoint, PolicyScope scope) {
  std::promise<void> promise;
  auto future = promise.get_future();
  auto session = endpoint.lock();
  _TaskQueue.Push([this, &promise, pid, session, scope]() -> Omni::Fiber::Coroutine<void> {
    _PolicyEngine->AddPidEndpointRule(pid, session, scope);
    promise.set_value();
    co_return;
  });
  future.get();
}

void PlatformImpl::SetDefaultEndpoint(VpnEndpoint endpoint) {
  std::promise<void> promise;
  auto future = promise.get_future();
  auto session = endpoint.lock();
  _TaskQueue.Push([this, &promise, session]() -> Omni::Fiber::Coroutine<void> {
    _PolicyEngine->SetDefaultEndpoint(session);
    promise.set_value();
    co_return;
  });
  future.get();
}

void PlatformImpl::SetDefaultBypass() {
  std::promise<void> promise;
  auto future = promise.get_future();
  _TaskQueue.Push([this, &promise]() -> Omni::Fiber::Coroutine<void> {
    _PolicyEngine->SetDefaultBypass();
    promise.set_value();
    co_return;
  });
  future.get();
}

void PlatformImpl::LaunchWithPolicy(const std::string& command_line, VpnEndpoint endpoint, PolicyScope scope) {
  std::promise<void> promise;
  auto future = promise.get_future();
  auto session = endpoint.lock();
  _TaskQueue.Push([this, &promise, command_line, session, scope]() -> Omni::Fiber::Coroutine<void> {
    auto pid = _PolicyEngine->LaunchWithPolicy(command_line, session, scope);
    if (pid == 0) {
      promise.set_exception(std::make_exception_ptr(std::runtime_error("LaunchWithPolicy failed")));
    } else {
      promise.set_value();
    }
    co_return;
  });
  future.get();
}

auto PlatformImpl::Inject(Packet&& packet) -> Omni::Fiber::Coroutine<void> {
  if (_DataPlane) {
    co_await _DataPlane->Inject(std::move(packet));
  }
  co_return;
}

auto CreatePlatform(DataPlaneCallbacks& callbacks) -> std::shared_ptr<PlatformInterface> {
  return std::make_shared<PlatformImpl>(callbacks);
}

} // namespace gh::Interface
