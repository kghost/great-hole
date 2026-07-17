#include "WinDivertFlowSniffer.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/log/trivial.hpp>
#include <span>
#include <utility>

#include <windivert.h>
#include <windows.h>

#include "Asio.hpp"

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
      WinDivertOpen("true", WINDIVERT_LAYER_FLOW, 0, WINDIVERT_FLAG_SNIFF | WINDIVERT_FLAG_RECV_ONLY);
  if (_WinDivertFlowHandle == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    BOOST_LOG_TRIVIAL(error) << "WinDivertFlowSniffer: WinDivertOpen failed: " << err;
    co_return SysError(err);
  }

  _ReadEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (_ReadEvent == nullptr) {
    DWORD err = GetLastError();
    BOOST_LOG_TRIVIAL(error) << "WinDivertFlowSniffer: CreateEventW failed: " << err;
    WinDivertClose(_WinDivertFlowHandle);
    _WinDivertFlowHandle = INVALID_HANDLE_VALUE;
    co_return SysError(err);
  }

  _ReadObject.emplace(_Executor, _ReadEvent);
  co_return ErrorCode{};
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
          BOOST_LOG_TRIVIAL(error) << "WinDivertFlowSniffer: GetOverlappedResult failed: " << GetLastError();
          co_return;
        }
      } else {
        BOOST_LOG_TRIVIAL(error) << "WinDivertFlowSniffer: WinDivertRecvEx failed: " << err;
        co_return;
      }
    }

    auto ProcessFlowEvent = [&]() -> Omni::Fiber::Coroutine<void> {
      if (addr.Layer != WINDIVERT_LAYER_FLOW) {
        co_return;
      }

      auto ToV6 = [](std::span<const UINT32, 4> addrSpan) -> boost::asio::ip::address_v6 {
        boost::asio::ip::address_v6::bytes_type raw_bytes;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        WinDivertHelperHtonIPv6Address(addrSpan.data(), reinterpret_cast<UINT*>(raw_bytes.data()));
        return boost::asio::ip::address_v6(raw_bytes);
      };

      const auto& flow = addr.Flow; // NOLINT(cppcoreguidelines-pro-type-union-access)
      std::optional<ConnectionTracker::ConnectionKey> conn;

      if (flow.Protocol == IPPROTO_TCP) {
        if (addr.IPv6 != 0) {
          conn = ConnectionTracker::Ip6TcpKey{.LocalAddress = ToV6(flow.LocalAddr),
                                              .RemoteAddress = ToV6(flow.RemoteAddr),
                                              .LocalPort = flow.LocalPort,
                                              .RemotePort = flow.RemotePort};
        } else {
          auto local = ToV6(flow.LocalAddr);
          auto remote = ToV6(flow.RemoteAddr);
          if (local.is_v4_mapped() && remote.is_v4_mapped()) {
            conn = ConnectionTracker::Ip4TcpKey{
                .LocalAddress = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, local),
                .RemoteAddress = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, remote),
                .LocalPort = flow.LocalPort,
                .RemotePort = flow.RemotePort};
          }
        }
      } else if (flow.Protocol == IPPROTO_UDP) {
        if (addr.IPv6 != 0) {
          conn = ConnectionTracker::Ip6UdpKey{.LocalAddress = ToV6(flow.LocalAddr),
                                              .RemoteAddress = ToV6(flow.RemoteAddr),
                                              .LocalPort = flow.LocalPort,
                                              .RemotePort = flow.RemotePort};
        } else {
          auto local = ToV6(flow.LocalAddr);
          auto remote = ToV6(flow.RemoteAddr);
          if (local.is_v4_mapped() && remote.is_v4_mapped()) {
            conn = ConnectionTracker::Ip4UdpKey{
                .LocalAddress = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, local),
                .RemoteAddress = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, remote),
                .LocalPort = flow.LocalPort,
                .RemotePort = flow.RemotePort};
          }
        }
      } else if (flow.Protocol == IPPROTO_ICMP) {
        auto local = ToV6(flow.LocalAddr);
        auto remote = ToV6(flow.RemoteAddr);
        if (local.is_v4_mapped() && remote.is_v4_mapped()) {
          // Note: WinDivert flow layer maps ICMP type to LocalPort and ICMP code to RemotePort.
          // The ICMP Echo ID is not exposed in the WFP ALE flow layer. Thus, we normalize the
          // connection key ID to 0, which is also normalized in FlowTracker when matching.
          conn = ConnectionTracker::IcmpKey{
              .LocalAddress = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, local),
              .RemoteAddress = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, remote),
              .Id = 0};
        }
      } else if (flow.Protocol == IPPROTO_ICMPV6) {
        // Note: Similar to ICMPv4, the ICMPv6 Echo ID is not exposed in the flow layer.
        conn = ConnectionTracker::Icmp6Key{
            .LocalAddress = ToV6(flow.LocalAddr), .RemoteAddress = ToV6(flow.RemoteAddr), .Id = 0};
      }

      if (conn.has_value()) {
        if (addr.Event == WINDIVERT_EVENT_FLOW_ESTABLISHED) {
          co_await _Callback.OnFlowEstablished(conn.value(), flow.ProcessId);
        } else if (addr.Event == WINDIVERT_EVENT_FLOW_DELETED) {
          co_await _Callback.OnFlowDeleted(conn.value());
        }
      } else {
        BOOST_LOG_TRIVIAL(warning) << "WinDivertFlowSniffer: Failed to create connection key for flow: " << addr.Event
                                   << ", " << flow.Protocol << " " << ToV6(flow.LocalAddr) << ":" << flow.LocalPort
                                   << " <-> " << ToV6(flow.RemoteAddr) << ":" << flow.RemotePort;
      }
    };

    co_await ProcessFlowEvent();
  }
}

} // namespace gh
