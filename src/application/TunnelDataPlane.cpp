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

TunnelDataPlane::TunnelDataPlane(boost::asio::any_io_executor executor,
                                 TunnelDataPlanePolicyResolverCallback& policyResolver,
                                 Interface::DataPlaneCallbacks& callbacks)
    : _Executor(std::move(executor)), _PolicyResolver(policyResolver), _Callbacks(callbacks),
      _ConnectionTracker(std::make_shared<ConnectionTracker>(_Executor)) {}

TunnelDataPlane::~TunnelDataPlane() { assert(!_Running); }

#ifndef _WIN32
auto TunnelDataPlane::Start(int tunFd, int mtu, std::vector<char> encryptionKey) -> Omni::Fiber::Coroutine<ErrorCode>
#else
auto TunnelDataPlane::Start(int mtu, std::vector<char> encryptionKey) -> Omni::Fiber::Coroutine<ErrorCode>
#endif
{
  if (_Running) {
    co_return ErrorCode{};
  }
  _Running = true;
  _Callbacks.OnVpnStateChanged(TunnelState::Starting, "VPN starting");

#ifdef _WIN32
  auto tun = std::make_shared<WinDivert>(_Executor, "WinDivert", 0, 0, *this);
#else
  auto tun = std::make_shared<Tun>(_Executor, "AndroidTun", tunFd);
#endif

  auto udpDynMux = std::make_shared<UdpDynMux>(_Executor);
  auto filter = std::make_shared<FilterXor>(std::move(encryptionKey));
  _Client = std::make_shared<VpnClientMultiChannel>(_Executor, tun, udpDynMux, _ConnectionTracker, *this,
                                                    std::vector<std::shared_ptr<Filter>>{filter}, *this);
  auto err = co_await _Client->Start();
  if (err) {
    BOOST_LOG_TRIVIAL(error) << "Failed to start VpnClientMultiChannel: " << err.message();
    _Callbacks.OnVpnStateChanged(TunnelState::Failed, err.message());
    _Running = false;
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
  _Callbacks.OnVpnStateChanged(TunnelState::Stopped, "VPN tunnel stopped");
  co_return ErrorCode{};
}

auto TunnelDataPlane::AddEndpoint(const UdpDynMux::PskType& psk, const std::string& address)
    -> std::weak_ptr<VpnClientMultiChannelSession> {
  return _Client->RegisterChannel(psk, address);
}

void TunnelDataPlane::RemoveEndpoint(const std::weak_ptr<VpnClientMultiChannelSession>& weak) {
  _Client->UnregisterChannel(weak);
}

auto TunnelDataPlane::StartEndpoint(const std::weak_ptr<VpnClientMultiChannelSession>& weak)
    -> Omni::Fiber::Coroutine<void> {
  co_return co_await _Client->StartChannel(weak);
}

auto TunnelDataPlane::StopEndpoint(const std::weak_ptr<VpnClientMultiChannelSession>& weak)
    -> Omni::Fiber::Coroutine<void> {
  co_return co_await _Client->StopChannel(weak);
}

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

auto TunnelDataPlane::SelectConnectionMark(const ConnectionTracker::ConnectionKey& key)
    -> std::shared_ptr<ConnectionMark> {
  auto action = _PolicyResolver.ResolvePolicy(key);
  return std::visit(
      Overload{[](const Interface::PolicyRule::ByPassRoute&) -> std::shared_ptr<VpnClientMultiChannel::Mark> {
                 return std::make_unique<VpnClientMultiChannel::Mark>(VpnClientMultiChannel::Mark::Bypass{});
               },
               [](const Interface::PolicyRule::EndpointRoute& route) -> std::shared_ptr<VpnClientMultiChannel::Mark> {
                 if (auto session = route.Endpoint.lock()) {
                   return std::make_unique<VpnClientMultiChannel::Mark>(VpnClientMultiChannel::Mark::RouteVia{session});
                 } else {
                   return std::make_unique<VpnClientMultiChannel::Mark>(VpnClientMultiChannel::Mark::Discard{});
                 }
               }},
      action);
}

#ifdef _WIN32
auto TunnelDataPlane::WinDivertRoute(Packet& packet, const WINDIVERT_ADDRESS& addr) -> WinDivertRouteCallback::Result {
  if (addr.Loopback || !addr.Outbound) {
    return WinDivertRouteCallback::Result::Normal;
  }
  assert(_ConnectionTracker);
  auto result = _ConnectionTracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(packet, *this);
  if (result.has_value()) {
    auto mark = std::dynamic_pointer_cast<VpnClientMultiChannel::Mark>(result.value());

    return std::visit(Overload{
                          [](VpnClientMultiChannel::Mark::ToBeSelected) -> gh::WinDivertRouteCallback::Result {
                            assert(false && "should not reach here");
                            std::unreachable();
                          },
                          [](VpnClientMultiChannel::Mark::Bypass) -> gh::WinDivertRouteCallback::Result {
                            return WinDivertRouteCallback::Result::Bypass;
                          },
                          [](VpnClientMultiChannel::Mark::Discard) -> gh::WinDivertRouteCallback::Result {
                            return WinDivertRouteCallback::Result::Discard;
                          },
                          [&packet, &mark](const VpnClientMultiChannel::Mark::RouteVia& /*routeVia*/)
                              -> gh::WinDivertRouteCallback::Result {
                            packet.SetMark(mark);
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

auto ToFlowConnection(const ConnectionTracker::ConnectionKey& key) -> Interface::FlowConnection {
  return std::visit(Overload{
                        [](const ConnectionTracker::Ip4TcpKey& key) -> Interface::FlowConnection {
                          return {.Protocol = "TCPv4",
                                  .LocalAddress = key.LocalAddress.to_string(),
                                  .RemoteAddress = key.RemoteAddress.to_string(),
                                  .LocalPort = key.LocalPort,
                                  .RemotePort = key.RemotePort};
                        },
                        [](const ConnectionTracker::Ip6TcpKey& key) -> Interface::FlowConnection {
                          return {.Protocol = "TCPv6",
                                  .LocalAddress = key.LocalAddress.to_string(),
                                  .RemoteAddress = key.RemoteAddress.to_string(),
                                  .LocalPort = key.LocalPort,
                                  .RemotePort = key.RemotePort};
                        },
                        [](const ConnectionTracker::Ip4UdpKey& key) -> Interface::FlowConnection {
                          return {.Protocol = "UDPv4",
                                  .LocalAddress = key.LocalAddress.to_string(),
                                  .RemoteAddress = key.RemoteAddress.to_string(),
                                  .LocalPort = key.LocalPort,
                                  .RemotePort = key.RemotePort};
                        },
                        [](const ConnectionTracker::Ip6UdpKey& key) -> Interface::FlowConnection {
                          return {.Protocol = "UDPv6",
                                  .LocalAddress = key.LocalAddress.to_string(),
                                  .RemoteAddress = key.RemoteAddress.to_string(),
                                  .LocalPort = key.LocalPort,
                                  .RemotePort = key.RemotePort};
                        },
                        [](const ConnectionTracker::IcmpKey& key) -> Interface::FlowConnection {
                          return {.Protocol = "ICMPv4",
                                  .LocalAddress = key.LocalAddress.to_string(),
                                  .RemoteAddress = key.RemoteAddress.to_string(),
                                  .LocalPort = key.Id,
                                  .RemotePort = 0};
                        },
                        [](const ConnectionTracker::Icmp6Key& key) -> Interface::FlowConnection {
                          return {.Protocol = "ICMPv6",
                                  .LocalAddress = key.LocalAddress.to_string(),
                                  .RemoteAddress = key.RemoteAddress.to_string(),
                                  .LocalPort = key.Id,
                                  .RemotePort = 0};
                        },
                    },
                    key);
}

auto TunnelDataPlane::GetConnections() const -> std::vector<Interface::TrackedConnectionInfo> {
  if (!_ConnectionTracker) {
    return {};
  }
  auto entries = _ConnectionTracker->GetConnections();
  std::vector<Interface::TrackedConnectionInfo> result;
  result.reserve(entries.size());
  for (const auto& entry : entries) {
    result.push_back({.Connection = ToFlowConnection(entry.Key), .Mark = entry.Mark});
  }
  return result;
}

} // namespace gh
