#include "TunnelDataPlane.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/asio/ip/network_v4.hpp>
#include <boost/log/trivial.hpp>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "ConnectionTracker.hpp"
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

#ifdef _WIN32
TunnelDataPlane::TunnelDataPlane(boost::asio::any_io_executor executor,
                                 TunnelDataPlanePolicyResolverCallback& policyResolver,
                                 Interface::DataPlaneCallbacks& callbacks, std::span<Interface::IpAddress> addresses,
                                 int32_t mtu, WindowsLocalAddressMonitor& windowsLocalAddressMonitor)
    : _Executor(std::move(executor)), _PolicyResolver(policyResolver), _Callbacks(callbacks),
      _ConnectionTracker(std::make_shared<ConnectionTracker>(_Executor)),
      _WindowsLocalAddressMonitor(windowsLocalAddressMonitor) {
  for (const auto& address : addresses) {
    std::visit(Overload{[this](const Interface::Ip4Address& address) -> void {
                          _NatContext._Ip4Addresses.emplace_back(address.Bytes);
                        },
                        [this](const Interface::Ip6Address& address) -> void {
                          _NatContext._Ip6Addresses.emplace_back(address.Bytes);
                        }},
               address);
  }
  _NatContext._Mtu = mtu;
}

#else
TunnelDataPlane::TunnelDataPlane(boost::asio::any_io_executor executor,
                                 TunnelDataPlanePolicyResolverCallback& policyResolver,
                                 Interface::DataPlaneCallbacks& callbacks)
    : _Executor(std::move(executor)), _PolicyResolver(policyResolver), _Callbacks(callbacks),
      _ConnectionTracker(std::make_shared<ConnectionTracker>(_Executor)) {}
#endif

TunnelDataPlane::~TunnelDataPlane() { assert(!_Running); }

#ifndef _WIN32
auto TunnelDataPlane::Start(int tunFd, int mtu, std::vector<char> encryptionKey) -> Omni::Fiber::Coroutine<ErrorCode>
#else
auto TunnelDataPlane::Start(std::vector<char> encryptionKey) -> Omni::Fiber::Coroutine<ErrorCode>
#endif
{
  if (_Running) {
    co_return ErrorCode{};
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
  _Client = std::make_shared<VpnClientMultiChannel>(_Executor, _Callbacks, tun, udpDynMux, _ConnectionTracker, *this,
                                                    std::vector<std::shared_ptr<Filter>>{filter});
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

auto TunnelDataPlane::GetTrafficStats(const std::weak_ptr<VpnClientMultiChannelSession>& weak)
    -> std::optional<Interface::VpnTrafficStats> {
  return VpnClientMultiChannel::GetStats(weak);
}

auto TunnelDataPlane::Select(const ConnectionTracker::ConnectionKey& key) -> ConnectionTracker::Selector::Action {
  auto action = _PolicyResolver.ResolvePolicy(key);
  return std::visit(
      Overload{[](const Interface::PolicyRule::ByPassRoute&) -> ConnectionTracker::Selector::Action {
                 return Action(std::make_unique<VpnClientMultiChannel::Mark>(VpnClientMultiChannel::Mark::Bypass{}));
               },
               [](const Interface::PolicyRule::DiscardRoute&) -> ConnectionTracker::Selector::Action {
                 return Action(std::make_unique<VpnClientMultiChannel::Mark>(VpnClientMultiChannel::Mark::Discard{}));
               },
               [&](const Interface::PolicyRule::EndpointRoute& route) -> ConnectionTracker::Selector::Action {
                 if (auto session = route.Endpoint.lock()) {
#ifndef _WIN32
                   return Action(
                       std::make_unique<VpnClientMultiChannel::Mark>(VpnClientMultiChannel::Mark::RouteVia{session}));
#else
                   return std::visit(Overload{[&](const auto& key) -> Action {
                                       using KeyType = std::decay_t<decltype(key)>;
                                       if constexpr (std::is_same_v<KeyType, ConnectionTracker::Ip4TcpKey> ||
                                                     std::is_same_v<KeyType, ConnectionTracker::Ip4UdpKey> ||
                                                     std::is_same_v<KeyType, ConnectionTracker::IcmpKey>) {
                                         if (_NatContext._Ip4Addresses.empty()) {
                                           return Action(std::make_unique<VpnClientMultiChannel::Mark>(
                                               VpnClientMultiChannel::Mark::Discard{}));
                                         } else {
                                           return Action(std::make_unique<VpnClientMultiChannel::Mark>(
                                                             VpnClientMultiChannel::Mark::RouteVia{session}),
                                                         ConnectionTracker::Selector::Action::Snat4{
                                                             .LocalAddress = _NatContext._Ip4Addresses[0],
                                                             .LocalPort = std::nullopt});
                                         }
                                       } else if constexpr (std::is_same_v<KeyType, ConnectionTracker::Ip6TcpKey> ||
                                                            std::is_same_v<KeyType, ConnectionTracker::Ip6UdpKey> ||
                                                            std::is_same_v<KeyType, ConnectionTracker::Icmp6Key>) {
                                         if (_NatContext._Ip6Addresses.empty()) {
                                           return Action(std::make_unique<VpnClientMultiChannel::Mark>(
                                               VpnClientMultiChannel::Mark::Discard{}));
                                         } else {
                                           return Action(std::make_unique<VpnClientMultiChannel::Mark>(
                                                             VpnClientMultiChannel::Mark::RouteVia{session}),
                                                         ConnectionTracker::Selector::Action::Snat6{
                                                             .LocalAddress = _NatContext._Ip6Addresses[0],
                                                             .LocalPort = std::nullopt});
                                         }
                                       } else {
                                         static_assert(false);
                                         std::unreachable();
                                       }
                                     }},
                                     key);
#endif
                 } else {
                   return Action(std::make_unique<VpnClientMultiChannel::Mark>(VpnClientMultiChannel::Mark::Discard{}));
                 }
               }},
      action);
}

#ifdef _WIN32
namespace {
struct PacketAddressV4 {
  using NetworkType = boost::asio::ip::network_v4;
  boost::asio::ip::address_v4 Src;
  boost::asio::ip::address_v4 Dest;
};
struct PacketAddressV6 {
  using NetworkType = boost::asio::ip::network_v6;
  boost::asio::ip::address_v6 Src;
  boost::asio::ip::address_v6 Dest;
};

template <typename T>
concept PacketAddressTypes = (std::same_as<T, PacketAddressV4> || std::same_as<T, PacketAddressV6>);

auto GetPacketIpAddress(Packet& packet) -> auto {
  using Result = std::variant<std::monostate, PacketAddressV4, PacketAddressV6>;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return reinterpret_cast<IPHeader*>(packet.Data().data())
      ->As(packet.Data(), false,
           Overload{[](std::span<uint8_t> /*ip4span*/, IPv4Header* ip4) -> Result {
                      return PacketAddressV4{.Src = boost::asio::ip::make_address_v4(ip4->GetSrcIp()),
                                             .Dest = boost::asio::ip::make_address_v4(ip4->GetDestIp())};
                    },
                    [](std::span<uint8_t> /*ip6span*/, IPv6Header* ip6) -> Result {
                      return PacketAddressV6{.Src = boost::asio::ip::make_address_v6(ip6->SrcIp),
                                             .Dest = boost::asio::ip::make_address_v6(ip6->DestIp)};
                    },
                    [](std::span<uint8_t> /*span*/, std::string err) -> Result {
                      BOOST_LOG_TRIVIAL(info) << "GetPacketIpAddress: " << err;
                      return std::monostate{};
                    }});
}
template <PacketAddressTypes AddressType> auto InSameSubnet(const AddressType& address, int prefix) -> bool {
  using NetworkType = typename AddressType::NetworkType;
  return NetworkType(address.Src, prefix).canonical().address() ==
         NetworkType(address.Dest, prefix).canonical().address();
}
} // namespace

auto TunnelDataPlane::WinDivertRouteOutbound(Packet& packet) -> WinDivertRouteCallback::Result {
  auto pass = std::visit(Overload{
                             [](std::monostate /*unused*/) -> bool { return true; },
                             [&]<PacketAddressTypes T>(const T& address) -> bool {
                               auto addressInfo = _WindowsLocalAddressMonitor.GetAddressInfo(address.Src);
                               if (!addressInfo.has_value()) {
                                 return true; // Bypass packet which is not originating from our host
                               }

                               if (address.Dest.is_loopback()) {
                                 return true; // Bypass packets to loopback addresses
                               }

                               if (address.Dest.is_multicast()) {
                                 return true; // Bypass packets to multicast addresses
                               }

                               if constexpr (std::is_same_v<T, PacketAddressV6>) {
                                 if (address.Dest.is_link_local()) {
                                   return true; // Bypass packets to link local addresses
                                 }
                               }

                               if (InSameSubnet(address, addressInfo.value().PrefixLength)) {
                                 // TODO: may exclude DNS
                                 return true; // Bypass packets to local network
                               }

                               return false;
                             },
                         },
                         GetPacketIpAddress(packet));
  if (pass) {
    return WinDivertRouteCallback::Result::Bypass;
  }

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

auto TunnelDataPlane::WinDivertRouteInbound(Packet& packet) -> std::optional<InterfaceIndex> {
  return std::visit(Overload{
                        [](std::monostate /*unused*/) -> std::optional<InterfaceIndex> { return std::nullopt; },
                        [&]<PacketAddressTypes T>(const T& address) -> std::optional<InterfaceIndex> {
                          return _WindowsLocalAddressMonitor.GetAddressInfo(address.Dest)
                              .transform([](const auto& info) -> InterfaceIndex { return info.InterfaceIndex; });
                        },
                    },
                    GetPacketIpAddress(packet));
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
