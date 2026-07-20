#include "TunnelDataPlane.hpp"

#include <boost/asio.hpp>
#include <boost/log/trivial.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "Coroutine.hpp"
#include "EndpointUdpDynMux.hpp"
#include "ErrorCode.hpp"
#include "FilterXor.hpp"
#include "Utils/Overload.hpp"
#include "VpnClientMultiChannel.hpp"

#ifdef _WIN32
#include "EndpointWinDivert.hpp"
#else
#include "EndpointTun.hpp"
#endif

namespace gh {

using TunnelState = Interface::TunnelState;

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
    int mtu, std::vector<char> encryptionKey) -> Omni::Fiber::Coroutine<ErrorCode> {
  if (_Running) {
    co_return ErrorCode{};
  }
  _Running = true;
  _Callbacks.OnVpnStateChanged(TunnelState::Starting, "VPN starting");

#ifdef _WIN32
  _WinDivert = std::make_shared<WinDivert>(_Executor, "WinDivert", 0, 0, *this);
  auto tun = _WinDivert;
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
    co_return err;
  }

  _Callbacks.OnVpnStateChanged(TunnelState::Running, "VPN tunnel established");
  co_return ErrorCode{};
}

#ifndef _WIN32
auto TunnelDataPlane::MigrateTun(int tunFd) -> Omni::Fiber::Coroutine<void> {
  if (_Running) {
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

auto TunnelDataPlane::Stop() -> Omni::Fiber::Coroutine<ErrorCode> {
  if (!_Running) {
    co_return ErrorCode{};
  }
  _Running = false;
  _Callbacks.OnVpnStateChanged(TunnelState::Stopping, "VPN stopping");

  auto err = co_await _Client->Stop();
  if (err) {
    co_return err;
  }

  _Client.reset();
#ifdef _WIN32
  _WinDivert.reset();
#endif

  _Callbacks.OnVpnStateChanged(TunnelState::Stopped, "VPN tunnel stopped");
  co_return ErrorCode{};
}

auto TunnelDataPlane::AddEndpoint(const UdpDynMux::PskType& psk, const std::string& address)
    -> Omni::Fiber::Coroutine<std::weak_ptr<VpnClientMultiChannelSession>> {
  auto session = co_await _Client->RegisterChannel(psk, address);
  co_return session;
}

auto TunnelDataPlane::RemoveEndpoint(std::weak_ptr<VpnClientMultiChannelSession> session)
    -> Omni::Fiber::Coroutine<void> {
  if (auto sharedSession = session.lock()) {
    co_await _Client->UnregisterChannel(sharedSession);
  }
}

#ifdef _WIN32
auto TunnelDataPlane::WinDivertRoute(Packet& packet, const WINDIVERT_ADDRESS& addr) -> WinDivertRouteCallback::Result {
  if (addr.Loopback || !addr.Outbound) {
    return WinDivertRouteCallback::Result::Normal;
  }
  auto result = _ConnectionTracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(packet, _Selector);
  if (result.has_value()) {
    auto mark = std::dynamic_pointer_cast<VpnClientMultiChannel::Mark>(result.value());
    packet.SetMark(mark);

    return std::visit(Overload{
                          [](VpnClientMultiChannel::Mark::ToBeSelected) -> gh::WinDivertRouteCallback::Result {
                            return WinDivertRouteCallback::Result::Normal;
                          },
                          [](VpnClientMultiChannel::Mark::Bypass) -> gh::WinDivertRouteCallback::Result {
                            return WinDivertRouteCallback::Result::Bypass;
                          },
                          [](VpnClientMultiChannel::Mark::Discard) -> gh::WinDivertRouteCallback::Result {
                            return WinDivertRouteCallback::Result::Discard;
                          },
                          [&packet](VpnClientMultiChannel::Mark::Deferred& deferred) -> gh::WinDivertRouteCallback::Result {
                            deferred.Packets.push_back(std::move(packet));
                            return WinDivertRouteCallback::Result::Discard;
                          },
                          [](const std::weak_ptr<VpnClientMultiChannelSession>&) -> gh::WinDivertRouteCallback::Result {
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

auto TunnelDataPlane::GetTrafficStats(const std::shared_ptr<VpnClientMultiChannelSession>& session)
    -> std::optional<VpnTrafficStats> {
  return VpnClientMultiChannel::GetStats(session);
}

void TunnelDataPlane::OnSessionStarting(const std::weak_ptr<VpnClientMultiChannelSession>& session) {
  _Callbacks.OnTunnelStateChanged(Interface::VpnEndpoint{session}, TunnelState::Starting, "");
}

void TunnelDataPlane::OnSessionRunning(const std::weak_ptr<VpnClientMultiChannelSession>& session) {
  _Callbacks.OnTunnelStateChanged(Interface::VpnEndpoint{session}, TunnelState::Running, "");
}

void TunnelDataPlane::OnSessionStopping(const std::weak_ptr<VpnClientMultiChannelSession>& session) {
  _Callbacks.OnTunnelStateChanged(Interface::VpnEndpoint{session}, TunnelState::Stopping, "");
}

void TunnelDataPlane::OnSessionStopped(const std::weak_ptr<VpnClientMultiChannelSession>& session) {
  _Callbacks.OnTunnelStateChanged(Interface::VpnEndpoint{session}, TunnelState::Stopped, "");
}

void TunnelDataPlane::OnSessionFailed(const std::weak_ptr<VpnClientMultiChannelSession>& session,
                                      const std::string& error) {
  _Callbacks.OnTunnelStateChanged(Interface::VpnEndpoint{session}, TunnelState::Failed, error);
}

} // namespace gh
