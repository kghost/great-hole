#include "EndpointWinDivert.hpp"

#include <boost/log/trivial.hpp>
#include <format>
#include <utility>

#include <windivert.h>

#include "Asio.hpp"
#include "Coroutine.hpp"
#include "ErrorCode.hpp"

namespace gh {

WinDivert::WinDivert(boost::asio::any_io_executor executor, std::string name, WinDivertRouteCallback& callback)
    : _Executor(std::move(executor)), _Name(std::move(name)), _RouteCallback(callback) {}

WinDivert::~WinDivert() { assert(_WinDivertHandle == INVALID_HANDLE_VALUE); }

auto WinDivert::GetName() const -> std::string { return std::format("WinDivert:{}[{}]", _Name, _WinDivertHandle); }

auto WinDivert::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> {
  _WinDivertHandle = WinDivertOpen("outbound and !impostor and !loopback and (ip or ipv6)", WINDIVERT_LAYER_NETWORK,
                                   0, // priority
                                   0  // flags
  );

  if (_WinDivertHandle == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    BOOST_LOG_TRIVIAL(error) << GetName() << ": WinDivertOpen failed with error: " << err;
    co_return SysError(err);
  }

  _ReadEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  _WriteEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

  if (_ReadEvent == nullptr || _WriteEvent == nullptr) {
    DWORD err = GetLastError();
    BOOST_LOG_TRIVIAL(error) << GetName() << ": CreateEvent failed with error: " << err;
    if (_ReadEvent != nullptr) {
      CloseHandle(_ReadEvent);
      _ReadEvent = nullptr;
    }
    if (_WriteEvent != nullptr) {
      CloseHandle(_WriteEvent);
      _WriteEvent = nullptr;
    }
    WinDivertClose(_WinDivertHandle);
    _WinDivertHandle = INVALID_HANDLE_VALUE;
    co_return SysError(err);
  }

  _ReadObjectHandle.emplace(_Executor, _ReadEvent);
  _WriteObjectHandle.emplace(_Executor, _WriteEvent);

  BOOST_LOG_TRIVIAL(info) << GetName() << ": Started successfully";
  co_return ErrorCode{};
}

auto WinDivert::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> {
  BOOST_LOG_TRIVIAL(info) << GetName() << ": Stopping...";

  if (_WinDivertHandle != INVALID_HANDLE_VALUE) {
    WinDivertClose(_WinDivertHandle);
    _WinDivertHandle = INVALID_HANDLE_VALUE;
  }

  if (_ReadObjectHandle.has_value()) {
    _ReadObjectHandle->cancel();
    _ReadObjectHandle->close();
    _ReadObjectHandle.reset();
    _ReadEvent = nullptr;
  }

  if (_WriteObjectHandle.has_value()) {
    _WriteObjectHandle->cancel();
    _WriteObjectHandle->close();
    _WriteObjectHandle.reset();
    _WriteEvent = nullptr;
  }

  co_await _PipielineUsageCounter.WaitAll();

  if (_ReadEvent != nullptr) {
    CloseHandle(_ReadEvent);
    _ReadEvent = nullptr;
  }

  if (_WriteEvent != nullptr) {
    CloseHandle(_WriteEvent);
    _WriteEvent = nullptr;
  }

  BOOST_LOG_TRIVIAL(info) << GetName() << ": Stopped successfully";
  co_return ErrorCode{};
}

auto WinDivert::Read(Packet& packet, Cancel& cancel) -> Omni::Fiber::Coroutine<ErrorCode> {
  OVERLAPPED overlapped = {};
  overlapped.hEvent = _ReadEvent;
  ResetEvent(_ReadEvent);
  Cancel::HandleTracker handleTracker(cancel, _WinDivertHandle, &overlapped);

  WINDIVERT_ADDRESS addr = {};
  UINT addrLen = sizeof(addr);
  UINT recvLen = 0;

  while (true) {
    if (cancel.IsTriggered()) {
      co_return Error(AppErrorCategory::kOperationAborted);
    }

    ResetEvent(_ReadEvent);
    addrLen = sizeof(addr);
    recvLen = 0;
    Packet winPacket;

    if (WinDivertRecvEx(_WinDivertHandle, winPacket.Data().data(), static_cast<UINT>(winPacket.Data().size()), &recvLen,
                        0, &addr, &addrLen, &overlapped) != TRUE) {
      DWORD err = GetLastError();
      if (err == ERROR_IO_PENDING) {
        auto [err2] = co_await _ReadObjectHandle->async_wait(Omni::Fiber::AsioUseFiber);
        if (err2) {
          CancelIoEx(_WinDivertHandle, &overlapped);
          DWORD transferred = 0;
          GetOverlappedResult(_WinDivertHandle, &overlapped, &transferred, TRUE);
          co_return err2;
        }

        DWORD transferred = 0;
        if (GetOverlappedResult(_WinDivertHandle, &overlapped, &transferred, FALSE) == TRUE) {
          recvLen = transferred;
        } else {
          co_return SysError(GetLastError());
        }
      } else {
        co_return SysError(err);
      }
    }

    winPacket._Length = recvLen;
    auto route = _RouteCallback.WinDivertRouteOutbound(winPacket);
    if (route == WinDivertRouteCallback::Result::Bypass) {
      UINT sendLen = 0;
      // FIXME: WinDivertSendEx may block
      if (WinDivertSendEx(_WinDivertHandle, winPacket.Data().data(), static_cast<UINT>(winPacket.Data().size()),
                          &sendLen, 0, &addr, sizeof(addr), nullptr) != TRUE) {
        DWORD err = GetLastError();
        BOOST_LOG_TRIVIAL(warning) << "WinDivert: bypass send failed: " << err;
      }
      continue;
    } else if (route == WinDivertRouteCallback::Result::Discard) {
      BOOST_LOG_TRIVIAL(info) << GetName() << ": Read packet size=" << winPacket.Data().size() << " Discarded";
      continue;
    } else {
      WinDivertHelperCalcChecksums(winPacket.Data().data(), static_cast<UINT>(winPacket.DataSize()), &addr, 0);
      packet = std::move(winPacket);
      co_return ErrorCode{};
    }
  }
}

auto WinDivert::Write(Packet& packet, Cancel& cancel) -> Omni::Fiber::Coroutine<ErrorCode> {
  if (cancel.IsTriggered()) {
    co_return Error(AppErrorCategory::kOperationAborted);
  }

  OVERLAPPED overlapped = {};
  overlapped.hEvent = _WriteEvent;
  ResetEvent(_WriteEvent);
  Cancel::HandleTracker handleTracker(cancel, _WinDivertHandle, &overlapped);

  auto interfaceIndex = _RouteCallback.WinDivertRouteInbound(packet);
  if (!interfaceIndex.has_value()) {
    BOOST_LOG_TRIVIAL(warning) << GetName() << ": Cannot route packet inbound, discarded";
    co_return Error(AppMinorErrorCategory::kSourceIpMismatch);
  }

  WINDIVERT_ADDRESS addr = {};
  addr.Layer = WINDIVERT_LAYER_NETWORK;
  addr.Outbound = 0;
  addr.Impostor = 1;
  addr.Network.IfIdx = interfaceIndex.value(); // NOLINT(cppcoreguidelines-pro-type-union-access)
  addr.Network.SubIfIdx = 0;                   // NOLINT(cppcoreguidelines-pro-type-union-access)

  UINT sendLen = 0;
  if (WinDivertSendEx(_WinDivertHandle, packet.Data().data(), static_cast<UINT>(packet.Data().size()), &sendLen, 0,
                      &addr, sizeof(addr), &overlapped) != TRUE) {
    DWORD err = GetLastError();
    if (err == ERROR_IO_PENDING) {
      auto [err2] = co_await _WriteObjectHandle->async_wait(Omni::Fiber::AsioUseFiber);
      if (err2) {
        CancelIoEx(_WinDivertHandle, &overlapped);
        DWORD transferred = 0;
        GetOverlappedResult(_WinDivertHandle, &overlapped, &transferred, TRUE);
        co_return err2;
      }

      DWORD transferred = 0;
      if (GetOverlappedResult(_WinDivertHandle, &overlapped, &transferred, FALSE) == 0) {
        co_return SysError(GetLastError());
      }
    } else {
      co_return SysError(err);
    }
  }

  co_return ErrorCode{};
}

} // namespace gh
