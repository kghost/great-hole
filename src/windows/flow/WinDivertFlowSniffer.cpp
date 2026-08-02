#include "WinDivertFlowSniffer.hpp"

#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/trivial.hpp>

#include <windows.h>

#include <winsock2.h>

#include <iphlpapi.h>
#include <windivert.h>
#include <ws2tcpip.h>

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
    auto err = SysError(GetLastError());
    BOOST_LOG_SEV(_Logger, boost::log::trivial::error)
        << "WinDivertFlowSniffer: WinDivertOpen failed: " << err.message();
    co_return err;
  }

  _ReadEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (_ReadEvent == nullptr) {
    auto err = SysError(GetLastError());
    BOOST_LOG_SEV(_Logger, boost::log::trivial::error)
        << "WinDivertFlowSniffer: CreateEventW failed: " << err.message();
    WinDivertClose(_WinDivertFlowHandle);
    _WinDivertFlowHandle = INVALID_HANDLE_VALUE;
    co_return err;
  }

  _ReadObject.emplace(_Executor, _ReadEvent);
  co_await BootstrapConnections();
  co_return ErrorCode{};
}

auto WinDivertFlowSniffer::BootstrapConnections() -> Omni::Fiber::Coroutine<void> {
  std::set<WinDivertFlowSnifferCallback::FlowKey> seenKeys;

  auto queryTable = [&]<typename QueryParameters>() -> Omni::Fiber::Coroutine<void> {
    for (const auto& [addr, port, pid] : QueryTablePid<QueryParameters>()) {
      WinDivertFlowSnifferCallback::FlowKey key = typename QueryParameters::KeyType{
          .Proto = QueryParameters::ResultProtocol, .LocalAddress = addr, .LocalPort = port};
      auto [iterator, inserted] = seenKeys.insert(key);
      if (inserted) {
        BOOST_LOG_SEV(_Logger, boost::log::trivial::trace)
            << "WinDivertFlowSniffer: Bootstrapped flow (" << key << "), PID: " << pid;
        if (pid > 0) {
          co_await _Callback.OnFlowEstablished(key, pid);
        }
      } else {
        BOOST_LOG_SEV(_Logger, boost::log::trivial::trace)
            << "WinDivertFlowSniffer: Already seen flow (" << key << "), PID: " << pid;
      }
    }
  };

  co_await queryTable.template operator()<QueryParametersTcp4>();
  co_await queryTable.template operator()<QueryParametersTcp6>();
  co_await queryTable.template operator()<QueryParametersUdp4>();
  co_await queryTable.template operator()<QueryParametersUdp6>();
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
      auto err = SysError(GetLastError());
      if (err.value() != ERROR_IO_PENDING) {
        BOOST_LOG_SEV(_Logger, boost::log::trivial::error)
            << "WinDivertFlowSniffer: WinDivertRecvEx failed: " << err.message();
        co_return;
      }

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
        auto err = SysError(GetLastError());
        BOOST_LOG_SEV(_Logger, boost::log::trivial::error)
            << "WinDivertFlowSniffer: GetOverlappedResult failed: " << err.message();
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
