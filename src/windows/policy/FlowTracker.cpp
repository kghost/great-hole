#include "FlowTracker.hpp"

#include <optional>
#include <variant>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/trivial.hpp>

#include <windows.h>

#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "Interface.hpp"
#include "Utils/Overload.hpp"

namespace gh::policy {

namespace {

auto QueryProcessFromSystemTables(const ConnectionTracker::ConnectionKey& key) -> std::optional<Interface::ProcessId> {
  auto findProcessInTable = [](auto&& gen, const auto& localAddr,
                               uint16_t localPort) -> std::optional<Interface::ProcessId> {
    std::optional<DWORD> wildcardPid;
    for (const auto& [addr, port, process] : gen) {
      if (port != localPort) {
        continue;
      }
      if (addr == localAddr) {
        return process;
      }
      if (addr.is_unspecified()) {
        wildcardPid = process;
      }
    }
    return wildcardPid;
  };

  return std::visit(
      Overload{
          [&](const ConnectionTracker::Ip4TcpKey& key) -> std::optional<Interface::ProcessId> {
            return findProcessInTable(WinDivertFlowSniffer::QueryTablePid<WinDivertFlowSniffer::QueryParametersTcp4>(),
                                      key.LocalAddress, key.LocalPort);
          },
          [&](const ConnectionTracker::Ip6TcpKey& key) -> std::optional<Interface::ProcessId> {
            return findProcessInTable(WinDivertFlowSniffer::QueryTablePid<WinDivertFlowSniffer::QueryParametersTcp6>(),
                                      key.LocalAddress, key.LocalPort);
          },
          [&](const ConnectionTracker::Ip4UdpKey& key) -> std::optional<Interface::ProcessId> {
            return findProcessInTable(WinDivertFlowSniffer::QueryTablePid<WinDivertFlowSniffer::QueryParametersUdp4>(),
                                      key.LocalAddress, key.LocalPort);
          },
          [&](const ConnectionTracker::Ip6UdpKey& key) -> std::optional<Interface::ProcessId> {
            return findProcessInTable(WinDivertFlowSniffer::QueryTablePid<WinDivertFlowSniffer::QueryParametersUdp6>(),
                                      key.LocalAddress, key.LocalPort);
          },
          [](const ConnectionTracker::IcmpKey&) -> std::optional<Interface::ProcessId> { return std::nullopt; },
          [](const ConnectionTracker::Icmp6Key&) -> std::optional<Interface::ProcessId> { return std::nullopt; },
      },
      key);
}

} // namespace

auto FlowTracker::GetProcessForConnection(const ConnectionTracker::ConnectionKey& key)
    -> std::optional<Interface::ProcessId> {
  auto flowKey = ToFlowExactKey(key);
  if (flowKey.has_value()) {
    auto iterator = _FlowToProcess.find(flowKey.value());
    if (iterator != _FlowToProcess.end()) {
      return iterator->second;
    }
  }

  auto flowWildcardKey = ToFlowWildcardKey(key);
  if (flowWildcardKey.has_value()) {
    auto iterator = _FlowToProcess.find(flowWildcardKey.value());
    if (iterator != _FlowToProcess.end()) {
      return iterator->second;
    }
  }

  BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
      << "FlowTracker: Connection " << key << " not in flow table, querying system TCP/UDP tables";
  auto process = QueryProcessFromSystemTables(key);
  if (process.has_value()) {
    BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
        << "FlowTracker: System table query returned PID " << process.value() << " for connection " << key;
    if (flowKey.has_value()) {
      _FlowToProcess[flowKey.value()] = process.value();
    }
    return process;
  } else {
    BOOST_LOG_SEV(_Logger, boost::log::trivial::warning)
        << "FlowTracker: System table query failed to find process sequence for connection " << key;
  }

  return std::nullopt;
}

auto FlowTracker::OnFlowEstablished(const FlowKey& key, Interface::ProcessId process) -> Omni::Fiber::Coroutine<void> {
  _FlowToProcess[key] = process;
  co_return;
}

auto FlowTracker::OnFlowDeleted(const FlowKey& key) -> Omni::Fiber::Coroutine<void> {
  _FlowToProcess.erase(key);
  co_return;
}

auto FlowTracker::GetFlows() const -> std::vector<Interface::FlowInfo> {
  std::vector<Interface::FlowInfo> flows;
  flows.reserve(_FlowToProcess.size());
  for (const auto& [key, process] : _FlowToProcess) {
    std::visit(Overload{[&](const auto& key) -> void {
                 flows.push_back({.Protocol = ProtocolToString(key.Proto),
                                  .LocalAddress = key.LocalAddress.to_string(),
                                  .LocalPort = key.LocalPort,
                                  .Process = process});
               }},
               key);
  }
  return flows;
}

[[nodiscard]] auto FlowTracker::ToFlowExactKey(const ConnectionTracker::ConnectionKey& key) -> std::optional<FlowKey> {
  return std::visit(
      Overload{
          [](const ConnectionTracker::Ip4TcpKey& key) -> std::optional<FlowKey> {
            return FlowIp4Key{.Proto = Protocol::Ipv4Tcp, .LocalAddress = key.LocalAddress, .LocalPort = key.LocalPort};
          },
          [](const ConnectionTracker::Ip6TcpKey& key) -> std::optional<FlowKey> {
            return FlowIp6Key{.Proto = Protocol::Ipv6Tcp, .LocalAddress = key.LocalAddress, .LocalPort = key.LocalPort};
          },
          [](const ConnectionTracker::Ip4UdpKey& key) -> std::optional<FlowKey> {
            return FlowIp4Key{.Proto = Protocol::Ipv4Udp, .LocalAddress = key.LocalAddress, .LocalPort = key.LocalPort};
          },
          [](const ConnectionTracker::Ip6UdpKey& key) -> std::optional<FlowKey> {
            return FlowIp6Key{.Proto = Protocol::Ipv6Udp, .LocalAddress = key.LocalAddress, .LocalPort = key.LocalPort};
          },
          [](const ConnectionTracker::IcmpKey&) -> std::optional<FlowKey> { return std::nullopt; },
          [](const ConnectionTracker::Icmp6Key&) -> std::optional<FlowKey> { return std::nullopt; },
      },
      key);
}

[[nodiscard]] auto FlowTracker::ToFlowWildcardKey(const ConnectionTracker::ConnectionKey& key)
    -> std::optional<FlowKey> {
  return std::visit(Overload{
                        [](const ConnectionTracker::Ip4TcpKey& key) -> std::optional<FlowKey> {
                          return FlowIp4Key{.Proto = Protocol::Ipv4Tcp,
                                            .LocalAddress = boost::asio::ip::address_v4::any(),
                                            .LocalPort = key.LocalPort};
                        },
                        [](const ConnectionTracker::Ip6TcpKey& key) -> std::optional<FlowKey> {
                          return FlowIp6Key{.Proto = Protocol::Ipv6Tcp,
                                            .LocalAddress = boost::asio::ip::address_v6::any(),
                                            .LocalPort = key.LocalPort};
                        },
                        [](const ConnectionTracker::Ip4UdpKey& key) -> std::optional<FlowKey> {
                          return FlowIp4Key{.Proto = Protocol::Ipv4Udp,
                                            .LocalAddress = boost::asio::ip::address_v4::any(),
                                            .LocalPort = key.LocalPort};
                        },
                        [](const ConnectionTracker::Ip6UdpKey& key) -> std::optional<FlowKey> {
                          return FlowIp6Key{.Proto = Protocol::Ipv6Udp,
                                            .LocalAddress = boost::asio::ip::address_v6::any(),
                                            .LocalPort = key.LocalPort};
                        },
                        [](const ConnectionTracker::IcmpKey&) -> std::optional<FlowKey> { return std::nullopt; },
                        [](const ConnectionTracker::Icmp6Key&) -> std::optional<FlowKey> { return std::nullopt; },
                    },
                    key);
}

} // namespace gh::policy