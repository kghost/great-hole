#pragma once

#include <array>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#if defined(__has_include) && __has_include(<generator>)
#include <generator>
#else
#include "Utils/Generator.hpp"
#endif

#include <boost/asio.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/asio/windows/object_handle.hpp>

#include <windows.h>

#include <winsock2.h>

#include <iphlpapi.h>
#include <ws2tcpip.h>

#include "InterfaceWin32.hpp"
#include "Logger.hpp"
#include "ServiceBase.hpp"
#include "Utils/Overload.hpp"

namespace gh {

class WinDivertFlowSnifferCallback {
public:
  explicit WinDivertFlowSnifferCallback() = default;
  virtual ~WinDivertFlowSnifferCallback() = default;

  WinDivertFlowSnifferCallback(const WinDivertFlowSnifferCallback&) = delete;
  auto operator=(const WinDivertFlowSnifferCallback&) -> WinDivertFlowSnifferCallback& = delete;
  WinDivertFlowSnifferCallback(WinDivertFlowSnifferCallback&&) = delete;
  auto operator=(WinDivertFlowSnifferCallback&&) -> WinDivertFlowSnifferCallback& = delete;

  enum class Protocol : uint8_t { Ipv4Tcp, Ipv4Udp, Ipv6Tcp, Ipv6Udp };

  static auto ProtocolToString(Protocol proto) -> std::string {
    switch (proto) {
    case Protocol::Ipv4Tcp:
      return "TCPv4";
    case Protocol::Ipv4Udp:
      return "UDPv4";
    case Protocol::Ipv6Tcp:
      return "TCPv6";
    case Protocol::Ipv6Udp:
      return "UDPv6";
    default:
      return "Unknown";
    }
  }

  struct FlowIp4Key {
    Protocol Proto{};
    boost::asio::ip::address_v4 LocalAddress;
    uint16_t LocalPort{0};
    auto operator<=>(const FlowIp4Key& other) const -> std::strong_ordering = default;
  };

  struct FlowIp6Key {
    Protocol Proto{};
    boost::asio::ip::address_v6 LocalAddress;
    uint16_t LocalPort{0};
    auto operator<=>(const FlowIp6Key& other) const -> std::strong_ordering = default;
  };

  using FlowKey = std::variant<FlowIp4Key, FlowIp6Key>;

  virtual auto OnFlowEstablished(const FlowKey& key, Interface::ProcessId pid) -> Omni::Fiber::Coroutine<void> = 0;
  virtual auto OnFlowDeleted(const FlowKey& key) -> Omni::Fiber::Coroutine<void> = 0;
};

class WinDivertFlowSniffer : public ServiceBase {
public:
  explicit WinDivertFlowSniffer(boost::asio::any_io_executor executor, WinDivertFlowSnifferCallback& callback);
  ~WinDivertFlowSniffer() override;

  WinDivertFlowSniffer(const WinDivertFlowSniffer&) = delete;
  auto operator=(const WinDivertFlowSniffer&) -> WinDivertFlowSniffer& = delete;
  WinDivertFlowSniffer(WinDivertFlowSniffer&&) = delete;
  auto operator=(WinDivertFlowSniffer&&) -> WinDivertFlowSniffer& = delete;

  auto GetName() const -> std::string override { return "WinDivertFlowSniffer"; }

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
    static constexpr auto ResultProtocol = WinDivertFlowSnifferCallback::Protocol::Ipv4Tcp;
    using KeyType = WinDivertFlowSnifferCallback::FlowIp4Key;
  };
  struct QueryParametersTcp6 : public QueryParametersV6, public QueryParametersTcp {
    using TableType = MIB_TCP6TABLE_OWNER_PID;
    static constexpr auto ResultProtocol = WinDivertFlowSnifferCallback::Protocol::Ipv6Tcp;
    using KeyType = WinDivertFlowSnifferCallback::FlowIp6Key;
  };
  struct QueryParametersUdp4 : public QueryParametersV4, public QueryParametersUdp {
    using TableType = MIB_UDPTABLE_OWNER_PID;
    static constexpr auto ResultProtocol = WinDivertFlowSnifferCallback::Protocol::Ipv4Udp;
    using KeyType = WinDivertFlowSnifferCallback::FlowIp4Key;
  };
  struct QueryParametersUdp6 : public QueryParametersV6, public QueryParametersUdp {
    using TableType = MIB_UDP6TABLE_OWNER_PID;
    static constexpr auto ResultProtocol = WinDivertFlowSnifferCallback::Protocol::Ipv6Udp;
    using KeyType = WinDivertFlowSnifferCallback::FlowIp6Key;
  };

  template <typename QueryParameters>
  static auto QueryTablePid() -> std::generator<std::tuple<typename QueryParameters::AddressType, uint16_t, DWORD>> {
    DWORD size = 0;
    QueryParameters::GetTable(nullptr, &size, FALSE, QueryParameters::AddressFamaly, QueryParameters::TableClass, 0);
    if (size == 0) {
      co_return;
    }
    std::vector<uint8_t> buffer(size);
    if (QueryParameters::GetTable(buffer.data(), &size, FALSE, QueryParameters::AddressFamaly,
                                  QueryParameters::TableClass, 0) != NO_ERROR) {
      co_return;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* table = reinterpret_cast<const QueryParameters::TableType*>(buffer.data());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    for (const auto& row : std::span(table->table, table->dwNumEntries)) {
      uint16_t rowPort = ntohs(static_cast<uint16_t>(row.dwLocalPort));
      auto rowAddr = QueryParameters::GetLocalAddress(row);
      co_yield std::make_tuple(rowAddr, rowPort, row.dwOwningPid);
    }
  }

protected:
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  auto BootstrapConnections() -> Omni::Fiber::Coroutine<void>;

  boost::asio::any_io_executor _Executor;
  WinDivertFlowSnifferCallback& _Callback;
  HANDLE _WinDivertFlowHandle = INVALID_HANDLE_VALUE;
  HANDLE _ReadEvent = nullptr;
  std::optional<boost::asio::windows::object_handle> _ReadObject;
  OVERLAPPED _Overlapped{};
  gh::base::ComponentLogger _Logger{boost::log::keywords::channel = "FlowTracker"};
};

inline auto operator<<(std::ostream& stream, const WinDivertFlowSnifferCallback::FlowKey& key) -> std::ostream& {
  return std::visit(Overload{[&](const auto& key) -> std::ostream& {
                      return stream << WinDivertFlowSnifferCallback::ProtocolToString(key.Proto) << ":"
                                    << key.LocalAddress << ":" << key.LocalPort;
                    }},
                    key);
}

} // namespace gh
