#include "PolicySelector.hpp"

#include <memory>
#include <variant>

#include <windows.h>
#include <ws2tcpip.h>

#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/trivial.hpp>

#include "PolicyRegistry.hpp"
#include "Utils/Overload.hpp"
#include "VpnClientMultiChannel.hpp"

namespace gh::policy {

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

PolicySelector::PolicySelector(boost::asio::any_io_executor& executor, PolicyRegistry& registry)
    : _TreeTracker(std::make_shared<ProcessTreeTracker>(executor, registry)) {}

auto PolicySelector::GetConnections() const -> std::vector<Interface::TrackedConnectionInfo> {
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

auto PolicySelector::SelectConnectionMark(const ConnectionTracker::ConnectionKey& key)
    -> std::shared_ptr<ConnectionMark> {
  return ResolvePolicy(key);
}

auto PolicySelector::ResolvePolicy(const ConnectionTracker::ConnectionKey& key) -> std::shared_ptr<ConnectionMark> {
  auto pid = _FlowTracker.GetPidForConnection(key);
  if (pid.has_value()) {
    auto action = _TreeTracker->GetAction(pid.value());
    if (action.has_value()) {
      BOOST_LOG_SEV(_Logger, boost::log::trivial::trace)
          << "ResolvePolicy: key=" << key << " pid=" << pid.value()
          << " resolved to action=" << PolicyActionToString(action.value());
      return ToConnectionMark(action.value());
    }
  }

  BOOST_LOG_SEV(_Logger, boost::log::trivial::trace)
      << "ResolvePolicy: key=" << key << " - no PID/action found, defaulting to bypass mark";
  return std::make_shared<VpnClientMultiChannel::Mark>(VpnClientMultiChannel::Mark::Bypass{});
}

auto PolicySelector::ToConnectionMark(const PolicyRule::RoutingAction& action)
    -> std::shared_ptr<VpnClientMultiChannel::Mark> {
  return std::visit(
      Overload{[](const PolicyRule::ByPassRoute&) -> std::shared_ptr<VpnClientMultiChannel::Mark> {
                 return std::make_unique<VpnClientMultiChannel::Mark>(VpnClientMultiChannel::Mark::Bypass{});
               },
               [](const PolicyRule::EndpointRoute& route) -> std::shared_ptr<VpnClientMultiChannel::Mark> {
                 if (auto session = route.Endpoint.lock()) {
                   return std::make_unique<VpnClientMultiChannel::Mark>(session);
                 } else {
                   return std::make_unique<VpnClientMultiChannel::Mark>(VpnClientMultiChannel::Mark::Discard{});
                 }
               }},
      action);
}

auto PolicySelector::WinDivertRoute(Packet& packet, const WINDIVERT_ADDRESS& addr) -> WinDivertRouteCallback::Result {
  if (addr.Loopback || !addr.Outbound) {
    return WinDivertRouteCallback::Result::Normal;
  }
  assert(_ConnectionTracker);
  auto result = _ConnectionTracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(packet, *this);
  if (result.has_value()) {
    auto mark = std::dynamic_pointer_cast<VpnClientMultiChannel::Mark>(result.value());

    return std::visit(
        Overload{
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
            [&packet, &mark](const std::weak_ptr<VpnClientMultiChannelSession>&) -> gh::WinDivertRouteCallback::Result {
              packet.SetMark(mark);
              return WinDivertRouteCallback::Result::Normal;
            },
        },
        mark->GetValue());
  } else {
    BOOST_LOG_SEV(_Logger, boost::log::trivial::warning)
        << "WinDivert: LookupAndUpdate bypass failed: " << result.error().message();
    return WinDivertRouteCallback::Result::Normal;
  }
}

} // namespace gh::policy
