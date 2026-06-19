#include "TunnelDataPlane.hpp"

#include <android/log.h>
#include <memory>
#include <string>
#include <unistd.h>

#include <boost/asio.hpp>
#include <boost/log/trivial.hpp>

#include "Coroutine.hpp"
#include "EndpointTun.hpp"
#include "EndpointUdpDynMux.hpp"
#include "FilterXor.hpp"
#include "ResolverHelper.hpp"
#include "VpnClientMultiChannel.hpp"

namespace gh {

TunnelDataPlane::TunnelDataPlane(boost::asio::any_io_executor executor, ConnectionTracker::SelectorType selector,
                                 DataPlaneCallbacks& callbacks)
    : _Executor(executor), _Selector(selector), _Callbacks(callbacks) {}

TunnelDataPlane::~TunnelDataPlane() { assert(!_Running); }

Omni::Fiber::Coroutine<void> TunnelDataPlane::Start(int tunFd, int mtu, std::vector<char> encryptionKey) {
  if (_Running) {
    co_return;
  }
  _Running = true;
  _Callbacks.OnVpnStateChanged(TunnelState::Starting, "VPN starting");

  _Tun = std::make_shared<Tun>(_Executor, "AndroidTun", tunFd);
  _UdpDynMux = std::make_shared<UdpDynMux>(_Executor);
  auto filter = std::make_shared<FilterXor>(std::move(encryptionKey));
  _Client = std::make_shared<VpnClientMultiChannel>(_Executor, _Tun, _UdpDynMux, _Selector,
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
      co_await newTun->WaitService();
      co_return;
    }

    if (_Tun) {
      co_await _Tun->Stop();
      co_await _Tun->WaitService();
    }
    _Tun = newTun;
  } else {
    ::close(tunFd);
  }
  co_return;
}

Omni::Fiber::Coroutine<void> TunnelDataPlane::Stop() {
  if (!_Running) {
    co_return;
  }
  _Running = false;
  _Callbacks.OnVpnStateChanged(TunnelState::Stopping, "VPN stopping");

  if (_Client) {
    co_await _Client->Stop();
    co_await _Client->WaitService();
  }

  if (_UdpDynMux) {
    co_await _UdpDynMux->Stop();
    co_await _UdpDynMux->WaitService();
  }

  if (_Tun) {
    co_await _Tun->Stop();
    co_await _Tun->WaitService();
  }

  _Client.reset();
  _UdpDynMux.reset();
  _Tun.reset();

  _Callbacks.OnVpnStateChanged(TunnelState::Stopped, "VPN tunnel stopped");
}

Omni::Fiber::Coroutine<VpnClientMultiChannel::Session*>
TunnelDataPlane::AddEndpoint(const UdpDynMux::PskType& psk, const std::string& host, int port) {
  std::shared_ptr<ResolverEndpoint> resolver;
  if (_UdpDynMux) {
    std::string addrStr = host + ":" + std::to_string(port);
    resolver = FindResolverEndpoint(addrStr, *_UdpDynMux);
  }
  if (_Client) {
    auto refSession = co_await _Client->RegisterChannel(psk, resolver);
    auto* sessionPtr = &refSession.get();
    _Endpoints.insert(sessionPtr);
    co_return sessionPtr;
  }
  co_return nullptr;
}

Omni::Fiber::Coroutine<void> TunnelDataPlane::RemoveEndpoint(VpnClientMultiChannel::Session* session) {
  if (session != nullptr) {
    _Endpoints.erase(session);
    if (_Client) {
      co_await _Client->UnregisterChannel(*session);
    }
  }
  co_return;
}

std::optional<std::reference_wrapper<ConnectionMark>>
TunnelDataPlane::FindSessionByHandle(VpnClientMultiChannel::Session* session) {
  if (_Endpoints.contains(session)) {
    return *session;
  }
  return std::nullopt;
}

std::optional<TrafficStats> TunnelDataPlane::GetTrafficStats(VpnClientMultiChannel::Session* session) {
  if (!_Client || !_Endpoints.contains(session)) {
    return std::nullopt;
  }
  return _Client->GetStats(*session);
}

void TunnelDataPlane::OnSessionStarting(VpnClientMultiChannel::Session& session) {
  _Callbacks.OnTunnelStateChanged(reinterpret_cast<int64_t>(&session), static_cast<int>(TunnelState::Starting), "");
}

void TunnelDataPlane::OnSessionRunning(VpnClientMultiChannel::Session& session) {
  _Callbacks.OnTunnelStateChanged(reinterpret_cast<int64_t>(&session), static_cast<int>(TunnelState::Running), "");
}

void TunnelDataPlane::OnSessionStopping(VpnClientMultiChannel::Session& session) {
  _Callbacks.OnTunnelStateChanged(reinterpret_cast<int64_t>(&session), static_cast<int>(TunnelState::Stopping), "");
}

void TunnelDataPlane::OnSessionStopped(VpnClientMultiChannel::Session& session) {
  _Callbacks.OnTunnelStateChanged(reinterpret_cast<int64_t>(&session), static_cast<int>(TunnelState::Stopped), "");
}

void TunnelDataPlane::OnSessionFailed(VpnClientMultiChannel::Session& session, const std::string& error) {
  _Callbacks.OnTunnelStateChanged(reinterpret_cast<int64_t>(&session), static_cast<int>(TunnelState::Failed), error);
}

} // namespace gh
