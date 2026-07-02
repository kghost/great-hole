#include "TunnelDataPlane.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <boost/asio.hpp>
#include <boost/log/trivial.hpp>

#include "Coroutine.hpp"
#include "EndpointUdpDynMux.hpp"
#include "FilterXor.hpp"
#include "ResolverHelper.hpp"
#include "VpnClientMultiChannel.hpp"

#if defined(_WIN32)
#include "EndpointWinDivert.hpp"
#else
#include "EndpointTun.hpp"
#endif

namespace gh {

TunnelDataPlane::TunnelDataPlane(boost::asio::any_io_executor executor, ConnectionTracker::Selector& selector,
                                 DataPlaneCallbacks& callbacks)
    : _Executor(executor), _Selector(selector), _Callbacks(callbacks) {}

TunnelDataPlane::~TunnelDataPlane() { assert(!_Running); }

Omni::Fiber::Coroutine<void> TunnelDataPlane::Start(
#if !defined(_WIN32)
    int tunFd,
#endif
    int mtu, std::vector<char> encryptionKey) {
  if (_Running) {
    co_return;
  }
  _Running = true;
  _Callbacks.OnVpnStateChanged(TunnelState::Starting, "VPN starting");

#if defined(_WIN32)
  _Tun = std::make_shared<EndpointWinDivert>(_Executor, "EndpointWinDivert");
#else
  _Tun = std::make_shared<Tun>(_Executor, "AndroidTun", tunFd);
#endif

  auto tracker = std::make_shared<ConnectionTracker>(_Executor, _Selector);
  _UdpDynMux = std::make_shared<UdpDynMux>(_Executor);
  auto filter = std::make_shared<FilterXor>(std::move(encryptionKey));
  _Client = std::make_shared<VpnClientMultiChannel>(_Executor, _Tun, _UdpDynMux, tracker,
                                                    std::vector<std::shared_ptr<Filter>>{filter}, *this);

  auto ec = co_await _Tun->Start();
  if (ec) {
    BOOST_LOG_TRIVIAL(error) << "Failed to start Tun: " << ec.message();
    _Callbacks.OnVpnStateChanged(TunnelState::Failed, ec.message().c_str());
    co_return;
  }

  ec = co_await _UdpDynMux->Start();
  if (ec) {
    BOOST_LOG_TRIVIAL(error) << "Failed to start UdpDynMux: " << ec.message();
    _Callbacks.OnVpnStateChanged(TunnelState::Failed, ec.message().c_str());
    co_return;
  }

  ec = co_await _Client->Start();
  if (ec) {
    BOOST_LOG_TRIVIAL(error) << "Failed to start VpnClientMultiChannel: " << ec.message();
    _Callbacks.OnVpnStateChanged(TunnelState::Failed, ec.message().c_str());
    co_return;
  }

  _Callbacks.OnVpnStateChanged(TunnelState::Running, "VPN tunnel established");
}

#if !defined(_WIN32)
Omni::Fiber::Coroutine<void> TunnelDataPlane::MigrateTun(int tunFd) {
  if (_Running && _Client) {
    auto newTun = std::make_shared<Tun>(_Executor, "AndroidTun", tunFd);
    auto ec = co_await newTun->Start();
    if (ec) {
      BOOST_LOG_TRIVIAL(error) << "Failed to start new Tun during migration: " << ec.message();
      ::close(tunFd);
      co_return;
    }

    ec = co_await _Client->MigrateTun(newTun);
    if (ec) {
      BOOST_LOG_TRIVIAL(error) << "Failed to migrate Tun in VpnClientMultiChannel: " << ec.message();
      co_await newTun->Stop();
      co_return;
    }

    if (_Tun) {
      co_await _Tun->Stop();
    }
    _Tun = newTun;
  } else {
    ::close(tunFd);
  }
  co_return;
}
#endif

Omni::Fiber::Coroutine<void> TunnelDataPlane::Stop() {
  if (!_Running) {
    co_return;
  }
  _Running = false;
  _Callbacks.OnVpnStateChanged(TunnelState::Stopping, "VPN stopping");

  if (_Client) {
    co_await _Client->Stop();
  }

  if (_UdpDynMux) {
    co_await _UdpDynMux->Stop();
  }

  if (_Tun) {
    co_await _Tun->Stop();
  }

  _Client.reset();
  _UdpDynMux.reset();
  _Tun.reset();

  _Callbacks.OnVpnStateChanged(TunnelState::Stopped, "VPN tunnel stopped");
}

Omni::Fiber::Coroutine<std::shared_ptr<VpnClientMultiChannel::Session>>
TunnelDataPlane::AddEndpoint(const UdpDynMux::PskType& psk, const std::string& host, int port) {
  std::shared_ptr<ResolverEndpoint> resolver;
  if (_UdpDynMux) {
    std::string addrStr = host + ":" + std::to_string(port);
    resolver = FindResolverEndpoint(addrStr, *_UdpDynMux);
  }
  if (_Client) {
    auto session = co_await _Client->RegisterChannel(psk, resolver);
    _Endpoints.insert(session);
    co_return session;
  }
  co_return nullptr;
}

Omni::Fiber::Coroutine<void> TunnelDataPlane::RemoveEndpoint(std::shared_ptr<VpnClientMultiChannel::Session> session) {
  _Endpoints.erase(session);
  if (_Client) {
    co_await _Client->UnregisterChannel(session);
  }
}

std::shared_ptr<VpnClientMultiChannel::Session>
TunnelDataPlane::FindSessionByHandle(VpnClientMultiChannel::Session* session) {
  auto it = _Endpoints.find(session);
  if (it != _Endpoints.end()) {
    return *it;
  }
  return nullptr;
}

std::optional<VpnTrafficStats>
TunnelDataPlane::GetTrafficStats(std::shared_ptr<VpnClientMultiChannel::Session> session) {
  return _Client->GetStats(session);
}

void TunnelDataPlane::OnSessionStarting(std::shared_ptr<VpnClientMultiChannel::Session> session) {
  _Callbacks.OnTunnelStateChanged(reinterpret_cast<int64_t>(session.get()), std::to_underlying(TunnelState::Starting),
                                  "");
}

void TunnelDataPlane::OnSessionRunning(std::shared_ptr<VpnClientMultiChannel::Session> session) {
  _Callbacks.OnTunnelStateChanged(reinterpret_cast<int64_t>(session.get()), std::to_underlying(TunnelState::Running),
                                  "");
}

void TunnelDataPlane::OnSessionStopping(std::shared_ptr<VpnClientMultiChannel::Session> session) {
  _Callbacks.OnTunnelStateChanged(reinterpret_cast<int64_t>(session.get()), std::to_underlying(TunnelState::Stopping),
                                  "");
}

void TunnelDataPlane::OnSessionStopped(std::shared_ptr<VpnClientMultiChannel::Session> session) {
  _Callbacks.OnTunnelStateChanged(reinterpret_cast<int64_t>(session.get()), std::to_underlying(TunnelState::Stopped),
                                  "");
}

void TunnelDataPlane::OnSessionFailed(std::shared_ptr<VpnClientMultiChannel::Session> session,
                                      const std::string& error) {
  _Callbacks.OnTunnelStateChanged(reinterpret_cast<int64_t>(session.get()), std::to_underlying(TunnelState::Failed),
                                  error);
}

} // namespace gh
