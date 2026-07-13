#include "TunnelDataPlane.hpp"
#include "Utils/Overload.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <boost/asio.hpp>
#include <boost/log/trivial.hpp>

#include "Coroutine.hpp"
#include "EndpointUdpDynMux.hpp"
#include "FilterXor.hpp"
#include "VpnClientMultiChannel.hpp"

#ifdef _WIN32
#include "EndpointWinDivert.hpp"
#else
#include "EndpointTun.hpp"
#endif

namespace gh {

TunnelDataPlane::TunnelDataPlane(boost::asio::any_io_executor executor, ConnectionTracker::Selector& selector,
                                 DataPlaneCallbacks& callbacks)
    : _Executor(std::move(executor)), _Selector(selector), _Callbacks(callbacks)
#ifdef _WIN32
      ,
      _ConnectionTracker(std::make_shared<ConnectionTracker>(_Executor))
#endif
{
}

TunnelDataPlane::~TunnelDataPlane() { assert(!_Running); }

auto TunnelDataPlane::Start(
#ifndef _WIN32
    int tunFd,
#endif
    int mtu, std::vector<char> encryptionKey) -> Omni::Fiber::Coroutine<void> {
  if (_Running) {
    co_return;
  }
  _Running = true;
  _Callbacks.OnVpnStateChanged(TunnelState::Starting, "VPN starting");

#ifdef _WIN32
  auto tun = std::make_shared<WinDivert>(_Executor, "WinDivert", *this);
#else
  auto tun = std::make_shared<Tun>(_Executor, "AndroidTun", tunFd);
#endif

  auto udpDynMux = std::make_shared<UdpDynMux>(_Executor);
  auto filter = std::make_shared<FilterXor>(std::move(encryptionKey));
#ifdef _WIN32
  _Client = std::make_shared<VpnClientMultiChannel>(_Executor, tun, udpDynMux, _ConnectionTracker, _Selector,
                                                    std::vector<std::shared_ptr<Filter>>{filter}, *this);
#else
  auto tracker = std::make_shared<ConnectionTracker>(_Executor);
  _Client = std::make_shared<VpnClientMultiChannel>(_Executor, tun, udpDynMux, tracker, _Selector,
                                                    std::vector<std::shared_ptr<Filter>>{filter}, *this);
#endif

  auto err = co_await _Client->Start();
  if (err) {
    BOOST_LOG_TRIVIAL(error) << "Failed to start VpnClientMultiChannel: " << err.message();
    _Callbacks.OnVpnStateChanged(TunnelState::Failed, err.message());
    co_return;
  }

  _Callbacks.OnVpnStateChanged(TunnelState::Running, "VPN tunnel established");
}

#ifndef _WIN32
auto TunnelDataPlane::MigrateTun(int tunFd) -> Omni::Fiber::Coroutine<void> {
  if (_Running && _Client) {
    auto newTun = std::make_shared<Tun>(_Executor, "AndroidTun", tunFd);
    auto err = co_await _Client->MigrateTun(newTun);
    if (err) {
      BOOST_LOG_TRIVIAL(error) << "Failed to migrate Tun in VpnClientMultiChannel: " << err.message();
      co_return;
    }
  } else {
    ::close(tunFd);
  }
  co_return;
}
#endif

auto TunnelDataPlane::Stop() -> Omni::Fiber::Coroutine<void> {
  if (!_Running) {
    co_return;
  }
  _Running = false;
  _Callbacks.OnVpnStateChanged(TunnelState::Stopping, "VPN stopping");

  if (_Client) {
    co_await _Client->Stop();
  }

  _Client.reset();

  _Callbacks.OnVpnStateChanged(TunnelState::Stopped, "VPN tunnel stopped");
}

auto TunnelDataPlane::AddEndpoint(const UdpDynMux::PskType& psk, const std::string& address)
    -> Omni::Fiber::Coroutine<std::shared_ptr<VpnClientMultiChannel::Session>> {
  if (_Client) {
    auto session = co_await _Client->RegisterChannel(psk, address);
    _Endpoints.insert(session);
    co_return session;
  }
  co_return nullptr;
}

auto TunnelDataPlane::RemoveEndpoint(std::shared_ptr<VpnClientMultiChannel::Session> session)
    -> Omni::Fiber::Coroutine<void> {
  _Endpoints.erase(session);
  if (_Client) {
    co_await _Client->UnregisterChannel(session);
  }
}

auto TunnelDataPlane::FindSessionByHandle(VpnClientMultiChannel::Session* session)
    -> std::shared_ptr<VpnClientMultiChannel::Session> {
  auto iterator = _Endpoints.find(session);
  if (iterator != _Endpoints.end()) {
    return *iterator;
  }
  return nullptr;
}

#ifdef _WIN32
auto TunnelDataPlane::WinDivertRoute(Packet& packet, const WINDIVERT_ADDRESS& addr) -> WinDivertRouteCallback::Result {
  if (addr.Loopback || !addr.Outbound) {
    return WinDivertRouteCallback::Result::Normal;
  }
  auto result = _ConnectionTracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionInput>(packet, _Selector);
  if (result.has_value()) {
    auto mark = std::dynamic_pointer_cast<VpnClientMultiChannel::Mark>(result.value());
    packet.SetMark(mark);

    return std::visit(
        Overload{
            [](VpnClientMultiChannel::Mark::ToBeSelected) -> gh::WinDivertRouteCallback::Result {
              return WinDivertRouteCallback::Result::Normal;
            },
            [](VpnClientMultiChannel::Mark::Bypass) -> gh::WinDivertRouteCallback::Result {
              return WinDivertRouteCallback::Result::Bypass;
            },
            [](VpnClientMultiChannel::Mark::Discard) -> gh::WinDivertRouteCallback::Result {
              return WinDivertRouteCallback::Result::Discard;
            },
            [](const VpnClientMultiChannel::Mark::Deferred&) -> gh::WinDivertRouteCallback::Result {
              return WinDivertRouteCallback::Result::Discard;
            },
            [](const std::weak_ptr<VpnClientMultiChannel::Session>&) -> gh::WinDivertRouteCallback::Result {
              return WinDivertRouteCallback::Result::Normal;
            },
        },
        mark->GetValue());
  } else {
    BOOST_LOG_TRIVIAL(warning) << "WinDivert: LookupAndUpdate bypass failed: " << result.error().message();
    return WinDivertRouteCallback::Result::Normal;
  }
}
#endif

auto TunnelDataPlane::GetTrafficStats(const std::shared_ptr<VpnClientMultiChannel::Session>& session)
    -> std::optional<VpnTrafficStats> {
  return VpnClientMultiChannel::GetStats(session);
}

void TunnelDataPlane::OnSessionStarting(const std::shared_ptr<VpnClientMultiChannel::Session>& session) {
  _Callbacks.OnTunnelStateChanged(session, TunnelState::Starting, "");
}

void TunnelDataPlane::OnSessionRunning(const std::shared_ptr<VpnClientMultiChannel::Session>& session) {
  _Callbacks.OnTunnelStateChanged(session, TunnelState::Running, "");
}

void TunnelDataPlane::OnSessionStopping(const std::shared_ptr<VpnClientMultiChannel::Session>& session) {
  _Callbacks.OnTunnelStateChanged(session, TunnelState::Stopping, "");
}

void TunnelDataPlane::OnSessionStopped(const std::shared_ptr<VpnClientMultiChannel::Session>& session) {
  _Callbacks.OnTunnelStateChanged(session, TunnelState::Stopped, "");
}

void TunnelDataPlane::OnSessionFailed(const std::shared_ptr<VpnClientMultiChannel::Session>& session,
                                      const std::string& error) {
  _Callbacks.OnTunnelStateChanged(session, TunnelState::Failed, error);
}

} // namespace gh
