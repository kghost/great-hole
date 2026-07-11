#include "EndpointWinDivert.hpp"

#include <boost/log/trivial.hpp>
#include <format>
#include <utility>
#include <windivert.h>

namespace gh {

WinDivert::WinDivert(boost::asio::any_io_executor executor, std::string name, WinDivertFastByPassCallback& callback)
    : _Executor(std::move(executor)), _Name(std::move(name)), _FastByPassCallback(callback) {}

WinDivert::~WinDivert() { assert(_WinDivertHandle == INVALID_HANDLE_VALUE); }

auto WinDivert::GetName() const -> std::string { return std::format("WinDivert:{}[{}]", _Name, _WinDivertHandle); }

auto WinDivert::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> {
  _WinDivertHandle = WinDivertOpen("outbound and !impostor and !loopback and (ip or ipv6)", WINDIVERT_LAYER_NETWORK,
                                   0, // priority
                                   0  // flags
  );

  if (_WinDivertHandle == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    BOOST_LOG_TRIVIAL(error) << "WinDivert: WinDivertOpen failed: " << err;
    co_return SysError(err);
  }

  _ReadEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  _WriteEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

  if ((_ReadEvent == nullptr) || (_WriteEvent == nullptr)) {
    DWORD err = GetLastError();
    BOOST_LOG_TRIVIAL(error) << "WinDivert: CreateEventW failed: " << err;
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

  BOOST_LOG_TRIVIAL(info) << "WinDivert: started, handle=" << _WinDivertHandle;
  co_return ErrorCode{};
}

auto WinDivert::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> {
  BOOST_LOG_TRIVIAL(info) << "WinDivert: stopping";

  if (_WinDivertHandle != INVALID_HANDLE_VALUE) {
    CancelIoEx(_WinDivertHandle, nullptr);
  }

  co_await _PipielineUsageCounter.WaitAll();

  if (_ReadObjectHandle.has_value()) {
    _ReadObjectHandle->close();
    _ReadObjectHandle.reset();
  }
  if (_WriteObjectHandle.has_value()) {
    _WriteObjectHandle->close();
    _WriteObjectHandle.reset();
  }

  if (_ReadEvent != nullptr) {
    CloseHandle(_ReadEvent);
    _ReadEvent = nullptr;
  }
  if (_WriteEvent != nullptr) {
    CloseHandle(_WriteEvent);
    _WriteEvent = nullptr;
  }

  if (_WinDivertHandle != INVALID_HANDLE_VALUE) {
    WinDivertClose(_WinDivertHandle);
    _WinDivertHandle = INVALID_HANDLE_VALUE;
  }

  BOOST_LOG_TRIVIAL(info) << "WinDivert: stopped";
  co_return ErrorCode{};
}

auto WinDivert::Read(Packet& packet, Cancel& cancel) -> Omni::Fiber::Coroutine<ErrorCode> {
  if (cancel.IsTriggered()) {
    co_return Error(AppErrorCategory::kOperationAborted);
  }

  OVERLAPPED overlapped = {};
  overlapped.hEvent = _ReadEvent;

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

    if (WinDivertRecvEx(_WinDivertHandle, packet.Data().data(), packet.Data().size(), &recvLen, 0, &addr, &addrLen,
                        &overlapped) != TRUE) {
      DWORD err = GetLastError();
      if (err == ERROR_IO_PENDING) {
        auto [err2] = co_await _ReadObjectHandle->async_wait(cancel.AsioSlot()());
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

    _LastIfIdx.store(addr.Network.IfIdx);       // NOLINT(cppcoreguidelines-pro-type-union-access)
    _LastSubIfIdx.store(addr.Network.SubIfIdx); // NOLINT(cppcoreguidelines-pro-type-union-access)

    packet._Length = recvLen;
    if (_FastByPassCallback.WinDivertShouldByPass(packet, addr)) {
      UINT sendLen = 0;
      if (WinDivertSendEx(_WinDivertHandle, packet.Data().data(), packet.Data().size(), &sendLen, 0, &addr,
                          sizeof(addr), nullptr) != TRUE) {
        DWORD err = GetLastError();
        BOOST_LOG_TRIVIAL(warning) << "WinDivert: bypass send failed: " << err;
      }
      packet._Length = packet._Data.size() - packet._Offset;
      continue;
    }
    co_return ErrorCode{};
  }
}

auto WinDivert::Write(Packet& packet, Cancel& cancel) -> Omni::Fiber::Coroutine<ErrorCode> {
  if (cancel.IsTriggered()) {
    co_return Error(AppErrorCategory::kOperationAborted);
  }

  OVERLAPPED overlapped = {};
  overlapped.hEvent = _WriteEvent;
  ResetEvent(_WriteEvent);

  WINDIVERT_ADDRESS addr = {};
  addr.Outbound = 0;
  addr.Layer = WINDIVERT_LAYER_NETWORK;
  addr.Network.IfIdx = _LastIfIdx.load();       // NOLINT(cppcoreguidelines-pro-type-union-access)
  addr.Network.SubIfIdx = _LastSubIfIdx.load(); // NOLINT(cppcoreguidelines-pro-type-union-access)

  UINT sendLen = 0;

  if (WinDivertSendEx(_WinDivertHandle, packet.Data().data(), packet.Data().size(), &sendLen, 0, &addr, sizeof(addr),
                      &overlapped) != TRUE) {
    DWORD err = GetLastError();
    if (err == ERROR_IO_PENDING) {
      auto [err2] = co_await _WriteObjectHandle->async_wait(cancel.AsioSlot()());
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
