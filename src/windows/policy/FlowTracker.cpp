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

#include "Utils/Overload.hpp"

namespace gh::policy {

namespace {

struct QueryParametersV4 {
  static constexpr auto AddressFamaly = AF_INET;
  using AddressType = boost::asio::ip::address_v4;
  static constexpr auto GetLocalAddress = [](const auto& row) -> AddressType {
    return AddressType(ntohl(row.dwLocalAddr));
  };
};
struct QueryParametersV6 {
  static constexpr auto AddressFamaly = AF_INET6;
  using AddressType = boost::asio::ip::address_v6;
  static constexpr auto GetLocalAddress = [](const auto& row) -> AddressType {
    return AddressType(std::to_array(row.ucLocalAddr));
  };
};
struct QueryParametersTcp {
  static constexpr auto GetTable =
      [](auto&&... args) -> decltype(GetExtendedTcpTable(std::forward<decltype(args)>(args)...)) {
    return GetExtendedTcpTable(std::forward<decltype(args)>(args)...);
  };
  static constexpr auto TableClass = TCP_TABLE_OWNER_PID_ALL;
};
struct QueryParametersUdp {
  static constexpr auto GetTable =
      [](auto&&... args) -> decltype(GetExtendedUdpTable(std::forward<decltype(args)>(args)...)) {
    return GetExtendedUdpTable(std::forward<decltype(args)>(args)...);
  };
  static constexpr auto TableClass = UDP_TABLE_OWNER_PID;
};

struct QueryParametersTcp4 : public QueryParametersV4, public QueryParametersTcp {
  using TableType = MIB_TCPTABLE_OWNER_PID;
};
struct QueryParametersTcp6 : public QueryParametersV6, public QueryParametersTcp {
  using TableType = MIB_TCP6TABLE_OWNER_PID;
};
struct QueryParametersUdp4 : public QueryParametersV4, public QueryParametersUdp {
  using TableType = MIB_UDPTABLE_OWNER_PID;
};
struct QueryParametersUdp6 : public QueryParametersV6, public QueryParametersUdp {
  using TableType = MIB_UDP6TABLE_OWNER_PID;
};

template <typename QueryParameters>
auto QueryTablePid(uint16_t localPort, const typename QueryParameters::AddressType& localAddr) -> std::optional<DWORD> {
  DWORD size = 0;
  QueryParameters::GetTable(nullptr, &size, FALSE, QueryParameters::AddressFamaly, QueryParameters::TableClass, 0);
  if (size == 0) {
    return std::nullopt;
  }
  std::vector<uint8_t> buffer(size);
  if (QueryParameters::GetTable(buffer.data(), &size, FALSE, QueryParameters::AddressFamaly,
                                QueryParameters::TableClass, 0) != NO_ERROR) {
    return std::nullopt;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* table = reinterpret_cast<const QueryParameters::TableType*>(buffer.data());
  std::optional<DWORD> wildcardPid;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
  for (const auto& row : std::span(table->table, table->dwNumEntries)) {
    uint16_t rowPort = ntohs(static_cast<uint16_t>(row.dwLocalPort));
    if (rowPort != localPort) {
      continue;
    }
    auto rowAddr = QueryParameters::GetLocalAddress(row);
    if (rowAddr == localAddr) {
      return row.dwOwningPid;
    }
    if (rowAddr.is_unspecified()) {
      wildcardPid = row.dwOwningPid;
    }
  }
  return wildcardPid;
}

auto QueryPidFromSystemTables(const ConnectionTracker::ConnectionKey& key) -> std::optional<DWORD> {
  return std::visit(
      Overload{
          [](const ConnectionTracker::Ip4TcpKey& key) -> std::optional<DWORD> {
            return QueryTablePid<QueryParametersTcp4>(key.LocalPort, key.LocalAddress)
                .or_else([&key]() -> std::optional<DWORD> {
                  return QueryTablePid<QueryParametersTcp6>(
                      key.LocalPort, boost::asio::ip::make_address_v6(boost::asio::ip::v4_mapped, key.LocalAddress));
                });
          },
          [](const ConnectionTracker::Ip6TcpKey& key) -> std::optional<DWORD> {
            return QueryTablePid<QueryParametersTcp6>(key.LocalPort, key.LocalAddress);
          },
          [](const ConnectionTracker::Ip4UdpKey& key) -> std::optional<DWORD> {
            return QueryTablePid<QueryParametersUdp4>(key.LocalPort, key.LocalAddress)
                .or_else([&key]() -> std::optional<DWORD> {
                  return QueryTablePid<QueryParametersUdp6>(
                      key.LocalPort, boost::asio::ip::make_address_v6(boost::asio::ip::v4_mapped, key.LocalAddress));
                });
          },
          [](const ConnectionTracker::Ip6UdpKey& key) -> std::optional<DWORD> {
            return QueryTablePid<QueryParametersUdp6>(key.LocalPort, key.LocalAddress);
          },
          [](const ConnectionTracker::IcmpKey&) -> std::optional<DWORD> { return std::nullopt; },
          [](const ConnectionTracker::Icmp6Key&) -> std::optional<DWORD> { return std::nullopt; },
      },
      key);
}

} // namespace

auto FlowTracker::GetPidForConnection(const ConnectionTracker::ConnectionKey& key) -> std::optional<DWORD> {
  auto flowKey = ToFlowExactKey(key);
  if (flowKey.has_value()) {
    auto iterator = _FlowToPid.find(flowKey.value());
    if (iterator != _FlowToPid.end()) {
      return iterator->second;
    }
  }

  auto flowWildcardKey = ToFlowWildcardKey(key);
  if (flowWildcardKey.has_value()) {
    auto iterator = _FlowToPid.find(flowWildcardKey.value());
    if (iterator != _FlowToPid.end()) {
      return iterator->second;
    }
  }

  BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
      << "FlowTracker: Connection " << key << " not in flow table, querying system TCP/UDP tables";
  auto pid = QueryPidFromSystemTables(key);
  if (pid.has_value()) {
    BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
        << "FlowTracker: System table query returned PID " << pid.value() << " for connection " << key;
    if (flowKey.has_value()) {
      _FlowToPid[flowKey.value()] = pid.value();
    }
    return pid;
  } else {
    BOOST_LOG_SEV(_Logger, boost::log::trivial::warning)
        << "FlowTracker: System table query failed to find PID for connection " << key;
  }

  return std::nullopt;
}

auto FlowTracker::OnFlowEstablished(const FlowKey& key, uint32_t pid) -> Omni::Fiber::Coroutine<void> {
  _FlowToPid[key] = pid;
  co_return;
}

auto FlowTracker::OnFlowDeleted(const FlowKey& key) -> Omni::Fiber::Coroutine<void> {
  _FlowToPid.erase(key);
  co_return;
}

auto FlowTracker::GetFlows() const -> std::vector<Interface::FlowInfo> {
  std::vector<Interface::FlowInfo> flows;
  flows.reserve(_FlowToPid.size());
  for (const auto& [key, pid] : _FlowToPid) {
    std::visit(Overload{[&](const auto& key) -> void {
                 flows.push_back({.Protocol = ProtocolToString(key.Proto),
                                  .LocalAddress = key.LocalAddress.to_string(),
                                  .LocalPort = key.LocalPort,
                                  .ProcessId = pid});
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