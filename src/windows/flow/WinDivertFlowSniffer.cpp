#include "WinDivertFlowSniffer.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/log/trivial.hpp>
#include <utility>

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

    const auto key = ([&] -> WinDivertFlowSnifferCallback::FlowKey {
      if (addr.IPv6 == 0) {
        return WinDivertFlowSnifferCallback::FlowKey{.Proto = flow.Protocol == IPPROTO_TCP
                                                                  ? WinDivertFlowSnifferCallback::Protocol::Ipv4Tcp
                                                                  : WinDivertFlowSnifferCallback::Protocol::Ipv4Udp,
                                                     .LocalPort = flow.LocalPort};
      } else {
        return WinDivertFlowSnifferCallback::FlowKey{.Proto = flow.Protocol == IPPROTO_TCP
                                                                  ? WinDivertFlowSnifferCallback::Protocol::Ipv6Tcp
                                                                  : WinDivertFlowSnifferCallback::Protocol::Ipv6Udp,
                                                     .LocalPort = flow.LocalPort};
      }
    })();

    if (addr.Event == WINDIVERT_EVENT_SOCKET_BIND) {
      BOOST_LOG_TRIVIAL(trace) << "WinDivertFlowSniffer: Flow established (" << key << "), PID: " << flow.ProcessId;
      co_await _Callback.OnFlowEstablished(key, flow.ProcessId);
    } else if (addr.Event == WINDIVERT_EVENT_SOCKET_CLOSE) {
      BOOST_LOG_TRIVIAL(trace) << "WinDivertFlowSniffer: Flow deleted (" << key << ")";
      co_await _Callback.OnFlowDeleted(key);
    }
  }
}

} // namespace gh
