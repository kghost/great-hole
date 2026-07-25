#include "WinDivertFlowSniffer.hpp"

#include <cstring>
#include <set>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/trivial.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windivert.h>
#include <windows.h>

namespace gh {

WinDivertFlowSniffer::WinDivertFlowSniffer(boost::asio::any_io_executor executor,
                                           WinDivertFlowSnifferCallback& callback)
    : _Executor(std::move(executor)), _Callback(callback) {}

WinDivertFlowSniffer::~WinDivertFlowSniffer() { assert(_WinDivertFlowHandle == INVALID_HANDLE_VALUE); }

auto WinDivertFlowSniffer::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> {
  if (_WinDivertFlowHandle != INVALID_HANDLE_VALUE) {
    co_return ErrorCode{};
  }

  _WinDivertFlowHandle =
      WinDivertOpen("true", WINDIVERT_LAYER_SOCKET, 0, WINDIVERT_FLAG_SNIFF | WINDIVERT_FLAG_RECV_ONLY);
  if (_WinDivertFlowHandle == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    BOOST_LOG_SEV(_Logger, boost::log::trivial::error) << "WinDivertFlowSniffer: WinDivertOpen failed: " << err;
    co_return SysError(err);
  }

  _ReadEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (_ReadEvent == nullptr) {
    DWORD err = GetLastError();
    BOOST_LOG_SEV(_Logger, boost::log::trivial::error) << "WinDivertFlowSniffer: CreateEventW failed: " << err;
    WinDivertClose(_WinDivertFlowHandle);
    _WinDivertFlowHandle = INVALID_HANDLE_VALUE;
    co_return SysError(err);
  }

  _ReadObject.emplace(_Executor, _ReadEvent);
  co_await BootstrapConnections();
  co_return ErrorCode{};
}

auto WinDivertFlowSniffer::BootstrapConnections() -> Omni::Fiber::Coroutine<void> {
  std::set<WinDivertFlowSnifferCallback::FlowKey> seenKeys;

  auto getTcpTable = [](ULONG af, TCP_TABLE_CLASS tableClass) -> std::vector<uint8_t> {
    DWORD size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, af, tableClass, 0);
    std::vector<uint8_t> buffer;
    DWORD ret = ERROR_INSUFFICIENT_BUFFER;
    for (int i = 0; i < 5 && ret == ERROR_INSUFFICIENT_BUFFER; ++i) {
      buffer.resize(size);
      ret = GetExtendedTcpTable(buffer.data(), &size, FALSE, af, tableClass, 0);
    }
    if (ret != NO_ERROR) {
      buffer.clear();
    }
    return buffer;
  };

  auto getUdpTable = [](ULONG af, UDP_TABLE_CLASS tableClass) -> std::vector<uint8_t> {
    DWORD size = 0;
    GetExtendedUdpTable(nullptr, &size, FALSE, af, tableClass, 0);
    std::vector<uint8_t> buffer;
    DWORD ret = ERROR_INSUFFICIENT_BUFFER;
    for (int i = 0; i < 5 && ret == ERROR_INSUFFICIENT_BUFFER; ++i) {
      buffer.resize(size);
      ret = GetExtendedUdpTable(buffer.data(), &size, FALSE, af, tableClass, 0);
    }
    if (ret != NO_ERROR) {
      buffer.clear();
    }
    return buffer;
  };

  // 1. TCP IPv4
  {
    auto buffer = getTcpTable(AF_INET, TCP_TABLE_OWNER_PID_ALL);
    if (!buffer.empty()) {
      const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
      for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        uint16_t port = ntohs(static_cast<uint16_t>(row.dwLocalPort));
        boost::asio::ip::address_v4::bytes_type bytes;
        std::memcpy(bytes.data(), &row.dwLocalAddr, 4);
        auto local4 = boost::asio::ip::address_v4(bytes);

        WinDivertFlowSnifferCallback::FlowKey key = WinDivertFlowSnifferCallback::FlowIp4Key{
            .Proto = WinDivertFlowSnifferCallback::Protocol::Ipv4Tcp,
            .LocalAddress = local4,
            .LocalPort = port};

        if (seenKeys.insert(key).second) {
          BOOST_LOG_SEV(_Logger, boost::log::trivial::trace)
              << "WinDivertFlowSniffer: Bootstrapped flow (" << key << "), PID: " << row.dwOwningPid;
          co_await _Callback.OnFlowEstablished(key, row.dwOwningPid);
        }
      }
    }
  }

  // 2. TCP IPv6
  {
    auto buffer = getTcpTable(AF_INET6, TCP_TABLE_OWNER_PID_ALL);
    if (!buffer.empty()) {
      const auto* table = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buffer.data());
      for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        uint16_t port = ntohs(static_cast<uint16_t>(row.dwLocalPort));
        boost::asio::ip::address_v6::bytes_type bytes;
        std::memcpy(bytes.data(), row.ucLocalAddr, 16);
        auto local6 = boost::asio::ip::address_v6(bytes);

        WinDivertFlowSnifferCallback::FlowKey key;
        if (local6.is_v4_mapped()) {
          auto local4 = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, local6);
          key = WinDivertFlowSnifferCallback::FlowIp4Key{
              .Proto = WinDivertFlowSnifferCallback::Protocol::Ipv4Tcp,
              .LocalAddress = local4,
              .LocalPort = port};
        } else {
          key = WinDivertFlowSnifferCallback::FlowIp6Key{
              .Proto = WinDivertFlowSnifferCallback::Protocol::Ipv6Tcp,
              .LocalAddress = local6,
              .LocalPort = port};
        }

        if (seenKeys.insert(key).second) {
          BOOST_LOG_SEV(_Logger, boost::log::trivial::trace)
              << "WinDivertFlowSniffer: Bootstrapped flow (" << key << "), PID: " << row.dwOwningPid;
          co_await _Callback.OnFlowEstablished(key, row.dwOwningPid);
        }
      }
    }
  }

  // 3. UDP IPv4
  {
    auto buffer = getUdpTable(AF_INET, UDP_TABLE_OWNER_PID);
    if (!buffer.empty()) {
      const auto* table = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(buffer.data());
      for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        uint16_t port = ntohs(static_cast<uint16_t>(row.dwLocalPort));
        boost::asio::ip::address_v4::bytes_type bytes;
        std::memcpy(bytes.data(), &row.dwLocalAddr, 4);
        auto local4 = boost::asio::ip::address_v4(bytes);

        WinDivertFlowSnifferCallback::FlowKey key = WinDivertFlowSnifferCallback::FlowIp4Key{
            .Proto = WinDivertFlowSnifferCallback::Protocol::Ipv4Udp,
            .LocalAddress = local4,
            .LocalPort = port};

        if (seenKeys.insert(key).second) {
          BOOST_LOG_SEV(_Logger, boost::log::trivial::trace)
              << "WinDivertFlowSniffer: Bootstrapped flow (" << key << "), PID: " << row.dwOwningPid;
          co_await _Callback.OnFlowEstablished(key, row.dwOwningPid);
        }
      }
    }
  }

  // 4. UDP IPv6
  {
    auto buffer = getUdpTable(AF_INET6, UDP_TABLE_OWNER_PID);
    if (!buffer.empty()) {
      const auto* table = reinterpret_cast<const MIB_UDP6TABLE_OWNER_PID*>(buffer.data());
      for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        uint16_t port = ntohs(static_cast<uint16_t>(row.dwLocalPort));
        boost::asio::ip::address_v6::bytes_type bytes;
        std::memcpy(bytes.data(), row.ucLocalAddr, 16);
        auto local6 = boost::asio::ip::address_v6(bytes);

        WinDivertFlowSnifferCallback::FlowKey key;
        if (local6.is_v4_mapped()) {
          auto local4 = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, local6);
          key = WinDivertFlowSnifferCallback::FlowIp4Key{
              .Proto = WinDivertFlowSnifferCallback::Protocol::Ipv4Udp,
              .LocalAddress = local4,
              .LocalPort = port};
        } else {
          key = WinDivertFlowSnifferCallback::FlowIp6Key{
              .Proto = WinDivertFlowSnifferCallback::Protocol::Ipv6Udp,
              .LocalAddress = local6,
              .LocalPort = port};
        }

        if (seenKeys.insert(key).second) {
          BOOST_LOG_SEV(_Logger, boost::log::trivial::trace)
              << "WinDivertFlowSniffer: Bootstrapped flow (" << key << "), PID: " << row.dwOwningPid;
          co_await _Callback.OnFlowEstablished(key, row.dwOwningPid);
        }
      }
    }
  }
}

auto WinDivertFlowSniffer::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> {
  if (_WinDivertFlowHandle != INVALID_HANDLE_VALUE) {
    CancelIoEx(_WinDivertFlowHandle, nullptr);
  }

  if (_ReadObject.has_value()) {
    _ReadObject->close();
    _ReadObject.reset();
    _ReadEvent = nullptr;
  }

  if (_ReadEvent != nullptr) {
    CloseHandle(_ReadEvent);
    _ReadEvent = nullptr;
  }

  if (_WinDivertFlowHandle != INVALID_HANDLE_VALUE) {
    WinDivertClose(_WinDivertFlowHandle);
    _WinDivertFlowHandle = INVALID_HANDLE_VALUE;
  }

  co_return ErrorCode{};
}

auto WinDivertFlowSniffer::DoWork() -> Omni::Fiber::Coroutine<void> {
  OVERLAPPED overlapped = {};
  overlapped.hEvent = _ReadEvent;
  Cancel::HandleTracker handleTracker(_Service.value()._Stop, _WinDivertFlowHandle, &overlapped);

  WINDIVERT_ADDRESS addr = {};
  UINT addrLen = sizeof(addr);
  UINT recvLen = 0;

  while (true) {
    if (_Service.value()._Stop.IsTriggered()) {
      co_return;
    }

    ResetEvent(_ReadEvent);
    addrLen = sizeof(addr);
    recvLen = 0;

    if (WinDivertRecvEx(_WinDivertFlowHandle, nullptr, 0, &recvLen, 0, &addr, &addrLen, &overlapped) != TRUE) {
      DWORD err = GetLastError();
      if (err == ERROR_IO_PENDING) {
        auto [errWinDivert] = co_await _ReadObject->async_wait(Omni::Fiber::AsioUseFiber);

        if (_Service.value()._Stop.IsTriggered()) {
          CancelIoEx(_WinDivertFlowHandle, &overlapped);
          DWORD transferred = 0;
          GetOverlappedResult(_WinDivertFlowHandle, &overlapped, &transferred, TRUE);
          co_return;
        }

        if (errWinDivert) {
          CancelIoEx(_WinDivertFlowHandle, &overlapped);
          DWORD transferred = 0;
          GetOverlappedResult(_WinDivertFlowHandle, &overlapped, &transferred, TRUE);
          co_return;
        }

        DWORD transferred = 0;
        if (GetOverlappedResult(_WinDivertFlowHandle, &overlapped, &transferred, FALSE) == TRUE) {
          // Success
        } else {
          BOOST_LOG_SEV(_Logger, boost::log::trivial::error)
              << "WinDivertFlowSniffer: GetOverlappedResult failed: " << GetLastError();
          co_return;
        }
      } else {
        BOOST_LOG_SEV(_Logger, boost::log::trivial::error) << "WinDivertFlowSniffer: WinDivertRecvEx failed: " << err;
        co_return;
      }
    }

    if (addr.Layer != WINDIVERT_LAYER_SOCKET) {
      continue;
    }
    if (addr.Event != WINDIVERT_EVENT_SOCKET_BIND && addr.Event != WINDIVERT_EVENT_SOCKET_CLOSE) {
      continue;
    }

    const auto& flow = addr.Socket; // NOLINT(cppcoreguidelines-pro-type-union-access)
    if (flow.Protocol != IPPROTO_TCP && flow.Protocol != IPPROTO_UDP) {
      continue;
    }

    const auto key = ([&] -> std::optional<WinDivertFlowSnifferCallback::FlowKey> {
      boost::asio::ip::address_v6::bytes_type address_v6_bytes;
      // NOLINTBEGIN(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
      // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
      WinDivertHelperHtonIPv6Address(flow.LocalAddr, reinterpret_cast<UINT*>(address_v6_bytes.data()));
      // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
      // NOLINTEND(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
      auto local6 = boost::asio::ip::address_v6(address_v6_bytes);
      if (addr.IPv6 == 0) {
        if (local6.is_v4_mapped()) {
          auto local4 = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, local6);
          return WinDivertFlowSnifferCallback::FlowIp4Key{
              .Proto = flow.Protocol == IPPROTO_TCP ? WinDivertFlowSnifferCallback::Protocol::Ipv4Tcp
                                                    : WinDivertFlowSnifferCallback::Protocol::Ipv4Udp,
              .LocalAddress = local4,
              .LocalPort = flow.LocalPort};
        } else {
          if (local6.is_unspecified()) {
            return WinDivertFlowSnifferCallback::FlowIp4Key{
                .Proto = flow.Protocol == IPPROTO_TCP ? WinDivertFlowSnifferCallback::Protocol::Ipv4Tcp
                                                      : WinDivertFlowSnifferCallback::Protocol::Ipv4Udp,
                .LocalAddress = boost::asio::ip::address_v4::any(),
                .LocalPort = flow.LocalPort};
          }
          BOOST_LOG_SEV(_Logger, boost::log::trivial::trace)
              << "WinDivertFlowSniffer: local address not mapped IPv4: " << local6;
          return std::nullopt;
        }
      } else {
        return WinDivertFlowSnifferCallback::FlowIp6Key{.Proto = flow.Protocol == IPPROTO_TCP
                                                                     ? WinDivertFlowSnifferCallback::Protocol::Ipv6Tcp
                                                                     : WinDivertFlowSnifferCallback::Protocol::Ipv6Udp,
                                                        .LocalAddress = local6,
                                                        .LocalPort = flow.LocalPort};
      }
    })();

    if (key.has_value()) {
      if (addr.Event == WINDIVERT_EVENT_SOCKET_BIND) {
        BOOST_LOG_SEV(_Logger, boost::log::trivial::trace)
            << "WinDivertFlowSniffer: Flow established (" << key.value() << "), PID: " << flow.ProcessId;
        co_await _Callback.OnFlowEstablished(key.value(), flow.ProcessId);
      } else if (addr.Event == WINDIVERT_EVENT_SOCKET_CLOSE) {
        BOOST_LOG_SEV(_Logger, boost::log::trivial::trace)
            << "WinDivertFlowSniffer: Flow deleted (" << key.value() << ")";
        co_await _Callback.OnFlowDeleted(key.value());
      }
    }
  }
}

} // namespace gh
