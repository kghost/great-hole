#include "EndpointWinDivert.hpp"

#include <format>

#include <boost/log/trivial.hpp>
#include <windivert.h>

namespace gh {

EndpointWinDivert::EndpointWinDivert(boost::asio::any_io_executor executor, std::string const& name)
    : _Executor(executor), _Name(name), _WinDivertHandle(INVALID_HANDLE_VALUE) {}

EndpointWinDivert::~EndpointWinDivert() { assert(_WinDivertHandle == INVALID_HANDLE_VALUE); }

std::string EndpointWinDivert::GetName() const {
  return std::format("EndpointWinDivert:{}[{}]", _Name, _WinDivertHandle);
}

Omni::Fiber::Coroutine<ErrorCode> EndpointWinDivert::DoStart() {
  _WinDivertHandle = WinDivertOpen("outbound and !impostor and ip and !loopback", WINDIVERT_LAYER_NETWORK,
                                   0, // priority
                                   0  // flags
  );

  if (_WinDivertHandle == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    BOOST_LOG_TRIVIAL(error) << "EndpointWinDivert: WinDivertOpen failed: " << err;
    co_return ErrorCode(err, system_category());
  }

  _ReadEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  _WriteEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

  if (!_ReadEvent || !_WriteEvent) {
    DWORD err = GetLastError();
    BOOST_LOG_TRIVIAL(error) << "EndpointWinDivert: CreateEventW failed: " << err;
    if (_ReadEvent) {
      CloseHandle(_ReadEvent);
      _ReadEvent = nullptr;
    }
    if (_WriteEvent) {
      CloseHandle(_WriteEvent);
      _WriteEvent = nullptr;
    }
    WinDivertClose(_WinDivertHandle);
    _WinDivertHandle = INVALID_HANDLE_VALUE;
    co_return ErrorCode(err, system_category());
  }

  _ReadObjectHandle.emplace(_Executor, _ReadEvent);
  _WriteObjectHandle.emplace(_Executor, _WriteEvent);

  BOOST_LOG_TRIVIAL(info) << "EndpointWinDivert: started, handle=" << _WinDivertHandle;
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> EndpointWinDivert::DoGracefulStop() {
  BOOST_LOG_TRIVIAL(info) << "EndpointWinDivert: stopping";

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

  if (_ReadEvent) {
    CloseHandle(_ReadEvent);
    _ReadEvent = nullptr;
  }
  if (_WriteEvent) {
    CloseHandle(_WriteEvent);
    _WriteEvent = nullptr;
  }

  if (_WinDivertHandle != INVALID_HANDLE_VALUE) {
    WinDivertClose(_WinDivertHandle);
    _WinDivertHandle = INVALID_HANDLE_VALUE;
  }

  BOOST_LOG_TRIVIAL(info) << "EndpointWinDivert: stopped";
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> EndpointWinDivert::Read(Packet& p, Cancel& c) {
  if (c.IsTriggered()) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }

  OVERLAPPED overlapped = {};
  overlapped.hEvent = _ReadEvent;

  WINDIVERT_ADDRESS addr = {};
  UINT addrLen = sizeof(addr);
  UINT recvLen = 0;

  while (true) {
    if (c.IsTriggered()) {
      co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
    }

    ResetEvent(_ReadEvent);
    addrLen = sizeof(addr);
    recvLen = 0;

    BOOL ok =
        WinDivertRecvEx(_WinDivertHandle, p.Data().data(), p.Data().size(), &recvLen, 0, &addr, &addrLen, &overlapped);

    if (!ok) {
      DWORD err = GetLastError();
      if (err == ERROR_IO_PENDING) {
        auto [ec] = co_await _ReadObjectHandle->async_wait(c.AsioSlot()());
        if (ec) {
          CancelIoEx(_WinDivertHandle, &overlapped);
          DWORD transferred = 0;
          GetOverlappedResult(_WinDivertHandle, &overlapped, &transferred, TRUE);
          co_return ec;
        }

        DWORD transferred = 0;
        if (GetOverlappedResult(_WinDivertHandle, &overlapped, &transferred, FALSE)) {
          recvLen = transferred;
        } else {
          co_return ErrorCode(GetLastError(), system_category());
        }
      } else {
        co_return ErrorCode(err, system_category());
      }
    }

    _LastIfIdx.store(addr.Network.IfIdx);
    _LastSubIfIdx.store(addr.Network.SubIfIdx);

    p._Length = recvLen;
    co_return ErrorCode{};
  }
}

Omni::Fiber::Coroutine<ErrorCode> EndpointWinDivert::Write(Packet& p, Cancel& c) {
  if (c.IsTriggered()) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }

  OVERLAPPED overlapped = {};
  overlapped.hEvent = _WriteEvent;
  ResetEvent(_WriteEvent);

  WINDIVERT_ADDRESS addr = {};
  addr.Outbound = 0;
  addr.Layer = WINDIVERT_LAYER_NETWORK;
  addr.Network.IfIdx = _LastIfIdx.load();
  addr.Network.SubIfIdx = _LastSubIfIdx.load();

  UINT sendLen = 0;
  BOOL ok = WinDivertSendEx(_WinDivertHandle, p.Data().data(), p.Data().size(), &sendLen, 0, &addr, sizeof(addr),
                            &overlapped);

  if (!ok) {
    DWORD err = GetLastError();
    if (err == ERROR_IO_PENDING) {
      auto [ec] = co_await _WriteObjectHandle->async_wait(c.AsioSlot()());
      if (ec) {
        CancelIoEx(_WinDivertHandle, &overlapped);
        DWORD transferred = 0;
        GetOverlappedResult(_WinDivertHandle, &overlapped, &transferred, TRUE);
        co_return ec;
      }

      DWORD transferred = 0;
      if (!GetOverlappedResult(_WinDivertHandle, &overlapped, &transferred, FALSE)) {
        co_return ErrorCode(GetLastError(), system_category());
      }
    } else {
      co_return ErrorCode(err, system_category());
    }
  }

  co_return ErrorCode{};
}

} // namespace gh
