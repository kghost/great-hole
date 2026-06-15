#include "TunnelDataPlane.hpp"

#include <android/log.h>
#include <chrono>
#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <boost/log/trivial.hpp>

#include "Coroutine.hpp"
#include "EndpointTun.hpp"
#include "EndpointUdpDynMux.hpp"
#include "ResolverHelper.hpp"
#include "VpnClientMultiChannel.hpp"

namespace gh {

TunnelDataPlane::TunnelDataPlane(boost::asio::any_io_executor executor, ConnectionTracker::SelectorType selector,
                                 DataPlaneCallbacks& callbacks)
    : _Executor(executor), _Selector(selector), _Callbacks(callbacks) {}

TunnelDataPlane::~TunnelDataPlane() { assert(!_Running); }

Omni::Fiber::Coroutine<void> TunnelDataPlane::Start(int tunFd, int mtu) {
  if (_Running) {
    co_return;
  }
  _Running = true;
  _Callbacks.UpdateState(TunnelState::Connecting, "VPN starting");

  _Tun = std::make_shared<Tun>(_Executor, tunFd);
  _UdpDynMux = std::make_shared<UdpDynMux>(_Executor);
  _Client = std::make_shared<VpnClientMultiChannel>(_Executor, _Tun, _UdpDynMux, _Selector);

  auto ec = co_await _Tun->Start();
  if (ec) {
    BOOST_LOG_TRIVIAL(error) << "Failed to start Tun: " << ec.message();
    _Callbacks.UpdateState(TunnelState::Failed, ec.message().c_str());
    co_return;
  }

  ec = co_await _UdpDynMux->Start();
  if (ec) {
    BOOST_LOG_TRIVIAL(error) << "Failed to start UdpDynMux: " << ec.message();
    _Callbacks.UpdateState(TunnelState::Failed, ec.message().c_str());
    co_return;
  }

  ec = co_await _Client->Start();
  if (ec) {
    BOOST_LOG_TRIVIAL(error) << "Failed to start VpnClientMultiChannel: " << ec.message();
    _Callbacks.UpdateState(TunnelState::Failed, ec.message().c_str());
    co_return;
  }

  _Callbacks.UpdateState(TunnelState::Connected, "VPN tunnel established");

  _StatsTimer = std::make_shared<boost::asio::steady_timer>(_Executor);
  StartStatsLoop();
}

Omni::Fiber::Coroutine<void> TunnelDataPlane::Stop() {
  if (!_Running) {
    co_return;
  }
  _Running = false;

  if (_StatsTimer) {
    _StatsTimer->cancel();
    _StatsTimer.reset();
  }

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

  _Callbacks.UpdateState(TunnelState::Disconnected, "VPN tunnel stopped");
}

Omni::Fiber::Coroutine<VpnClientMultiChannel::Session*>
TunnelDataPlane::AddEndpoint(const std::string& displayName, const std::string& host, int port) {
  auto psk = ParsePsk(displayName);

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

UdpDynMux::PskType TunnelDataPlane::ParsePsk(const std::string& displayName) {
  UdpDynMux::PskType psk{};
  if (displayName.length() == 32) {
    bool valid = true;
    for (size_t i = 0; i < 16; ++i) {
      std::string byteString = displayName.substr(i * 2, 2);
      char* end;
      long val = std::strtol(byteString.c_str(), &end, 16);
      if (*end != '\0') {
        valid = false;
        break;
      }
      psk[i] = static_cast<uint8_t>(val);
    }
    if (valid) {
      return psk;
    }
  }
  uint64_t hash1 = 14695981039346656037ULL;
  uint64_t hash2 = 14695981039346656037ULL;
  for (char c : displayName) {
    hash1 = (hash1 ^ c) * 1099511628211ULL;
  }
  for (size_t i = 0; i < displayName.length(); ++i) {
    hash2 = (hash2 ^ displayName[displayName.length() - 1 - i]) * 1099511628211ULL;
  }
  std::memcpy(psk.data(), &hash1, 8);
  std::memcpy(psk.data() + 8, &hash2, 8);
  return psk;
}

void TunnelDataPlane::StartStatsLoop() {
  if (!_StatsTimer) {
    return;
  }
  _StatsTimer->expires_after(std::chrono::seconds(2));
  _StatsTimer->async_wait([this](const boost::system::error_code& ec) {
    if (ec) {
      return;
    }
    ReportStats();
    StartStatsLoop();
  });
}

void TunnelDataPlane::ReportStats() {
  if (!_Client) {
    return;
  }
  for (auto* session : _Endpoints) {
    auto [tx, rx] = _Client->GetStats(*session);
    if (tx > 0 || rx > 0) {
      _Callbacks.OnTrafficStats(reinterpret_cast<int64_t>(session), tx, rx);
    }
  }
}

} // namespace gh
