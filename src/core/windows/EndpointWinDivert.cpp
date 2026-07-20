#include "EndpointWinDivert.hpp"

#include <boost/log/trivial.hpp>
#include <format>
#include <utility>
#include <windivert.h>

#include "Asio.hpp"
#include "Coroutine.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"

namespace gh {

WinDivert::WinDivert(boost::asio::any_io_executor executor, std::string name, uint32_t ifIdx, uint32_t ifSubIdx,
                     WinDivertRouteCallback& callback)
    : _Executor(std::move(executor)), _Name(std::move(name)), _IfIdx(ifIdx), _IfSubIdx(ifSubIdx),
      _RouteCallback(callback) {}

WinDivert::~WinDivert() {
  assert(_WinDivertHandle == INVALID_HANDLE_VALUE);
  if (!_InjectedPacketPipe.IsClosed()) {
    _InjectedPacketPipe.GetConsumer().DiscardAndClose();
  }
}

auto WinDivert::GetName() const -> std::string {
  return std::format("WinDivert:{}:{}:{}[{}]", _Name, _IfIdx, _IfSubIdx, _WinDivertHandle);
}

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

  if (!_InjectedPacketPipe.IsClosed()) {
    _InjectedPacketPipe.GetConsumer().DiscardAndClose();
  }

  if (_WinDivertHandle != INVALID_HANDLE_VALUE) {

    CancelIoEx(_WinDivertHandle, nullptr);
  }

  co_await _PipielineUsageCounter.WaitAll();

  if (_ReadObjectHandle.has_value()) {
    _ReadObjectHandle->close();
    _ReadObjectHandle.reset();
    _ReadEvent = nullptr;
  }
  if (_WriteObjectHandle.has_value()) {
    _WriteObjectHandle->close();
    _WriteObjectHandle.reset();
    _WriteEvent = nullptr;
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
  Cancel::HandleTracker handleTracker(cancel, _WinDivertHandle, &overlapped);

  WINDIVERT_ADDRESS addr = {};
  UINT addrLen = sizeof(addr);
  UINT recvLen = 0;

  while (true) {
    if (cancel.IsTriggered()) {
      co_return Error(AppErrorCategory::kOperationAborted);
    }

    if (_InjectedPacketPipe.GetConsumer().AwaitReady()) {
      auto res = _InjectedPacketPipe.GetConsumer().AwaitValue();
      if (res.has_value()) {
        auto injected = std::move(res.value());
        if (injected.Route == WinDivertRouteCallback::Result::Bypass) {
          UINT sendLen = 0;
          if (WinDivertSendEx(_WinDivertHandle, injected.Pkt.Data().data(), injected.Pkt.Data().size(), &sendLen, 0,
                              &injected.Addr, sizeof(injected.Addr), nullptr) != TRUE) {
            DWORD err = GetLastError();
            BOOST_LOG_TRIVIAL(warning) << "WinDivert: bypass injected send failed: " << err;
          }
          continue;
        } else if (injected.Route == WinDivertRouteCallback::Result::Discard) {
          continue;
        }
        packet = std::move(injected.Pkt);
        BOOST_LOG_TRIVIAL(trace) << GetName() << ": Read (injected) packet size=" << packet.Data().size();
        co_return ErrorCode{};
      }
    }

    ResetEvent(_ReadEvent);
    addrLen = sizeof(addr);
    recvLen = 0;
    Packet winPacket;

    if (WinDivertRecvEx(_WinDivertHandle, winPacket.Data().data(), static_cast<UINT>(winPacket.Data().size()), &recvLen,
                        0, &addr, &addrLen, &overlapped) != TRUE) {
      DWORD err = GetLastError();
      if (err == ERROR_IO_PENDING) {
        InjectedPacket injectedPacket;
        auto [hasInjectedPacket, errWinDivert] = co_await Omni::Fiber::Select(
            Omni::Fiber::SelectPair(_InjectedPacketPipe.GetConsumer(),
                                    [&](std::expected<InjectedPacket, Omni::Fiber::PipeClosed> res) -> bool {
                                      if (res.has_value()) {
                                        injectedPacket = std::move(res.value());
                                        return true;
                                      } else {
                                        return false;
                                      }
                                    }),
            Omni::Fiber::SelectPair(
                _ReadObjectHandle->async_wait(Omni::Fiber::AsioUseFiber),
                Omni::Fiber::AsioApply([&](boost::system::error_code err) -> auto { return err; })));

        if (hasInjectedPacket.has_value() && hasInjectedPacket.value()) {
          CancelIoEx(_WinDivertHandle, &overlapped);
          DWORD transferred = 0;
          GetOverlappedResult(_WinDivertHandle, &overlapped, &transferred, TRUE);

          if (injectedPacket.Route == WinDivertRouteCallback::Result::Bypass) {
            UINT sendLen = 0;
            if (WinDivertSendEx(_WinDivertHandle, injectedPacket.Pkt.Data().data(), injectedPacket.Pkt.Data().size(),
                                &sendLen, 0, &injectedPacket.Addr, sizeof(injectedPacket.Addr), nullptr) != TRUE) {
              DWORD err = GetLastError();
              BOOST_LOG_TRIVIAL(warning) << "WinDivert: bypass injected send failed: " << err;
            }
            continue;
          } else if (injectedPacket.Route == WinDivertRouteCallback::Result::Discard) {
            continue;
          }
          packet = std::move(injectedPacket.Pkt);
          BOOST_LOG_TRIVIAL(trace) << GetName() << ": Read (injected) packet size=" << packet.Data().size();
          co_return ErrorCode{};
        }

        if (errWinDivert.has_value() && errWinDivert.value()) {
          CancelIoEx(_WinDivertHandle, &overlapped);
          DWORD transferred = 0;
          GetOverlappedResult(_WinDivertHandle, &overlapped, &transferred, TRUE);
          co_return errWinDivert.value();
        }

        if (!hasInjectedPacket.has_value() && !errWinDivert.has_value()) {
          CancelIoEx(_WinDivertHandle, &overlapped);
          DWORD transferred = 0;
          GetOverlappedResult(_WinDivertHandle, &overlapped, &transferred, TRUE);
          co_return Error(AppErrorCategory::kOperationAborted);
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
    auto route = _RouteCallback.WinDivertRoute(winPacket, addr);
    if (route == WinDivertRouteCallback::Result::Bypass) {
      UINT sendLen = 0;
      if (WinDivertSendEx(_WinDivertHandle, winPacket.Data().data(), winPacket.Data().size(), &sendLen, 0, &addr,
                          sizeof(addr), nullptr) != TRUE) {
        DWORD err = GetLastError();
        BOOST_LOG_TRIVIAL(warning) << "WinDivert: bypass send failed: " << err;
      }
      continue;
    } else if (route == WinDivertRouteCallback::Result::Discard) {
      BOOST_LOG_TRIVIAL(trace) << GetName() << ": Read packet size=" << winPacket.Data().size() << " Discarded";
      continue;
    } else {
      packet = std::move(winPacket);
      co_return ErrorCode{};
    }
  }
}

auto WinDivert::Write(Packet& packet, Cancel& cancel) -> Omni::Fiber::Coroutine<ErrorCode> {
  if (cancel.IsTriggered()) {
    co_return Error(AppErrorCategory::kOperationAborted);
  }

  BOOST_LOG_TRIVIAL(trace) << GetName() << ": Write packet size=" << packet.Data().size();

  OVERLAPPED overlapped = {};
  overlapped.hEvent = _WriteEvent;
  ResetEvent(_WriteEvent);
  Cancel::HandleTracker handleTracker(cancel, _WinDivertHandle, &overlapped);

  WINDIVERT_ADDRESS addr = {};
  addr.Outbound = 0;
  addr.Layer = WINDIVERT_LAYER_NETWORK;
  addr.Network.IfIdx = _IfIdx;       // NOLINT(cppcoreguidelines-pro-type-union-access)
  addr.Network.SubIfIdx = _IfSubIdx; // NOLINT(cppcoreguidelines-pro-type-union-access)

  UINT sendLen = 0;

  if (WinDivertSendEx(_WinDivertHandle, packet.Data().data(), packet.Data().size(), &sendLen, 0, &addr, sizeof(addr),
                      &overlapped) != TRUE) {
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

auto WinDivert::Inject(Packet&& packet, const WINDIVERT_ADDRESS& addr, WinDivertRouteCallback::Result route)
    -> Omni::Fiber::Coroutine<void> {
  BOOST_LOG_TRIVIAL(trace) << GetName() << ": Injecting packet size=" << packet.Data().size();
  auto result = co_await _InjectedPacketPipe.GetProducer().Put(
      InjectedPacket{.Pkt = std::move(packet), .Addr = addr, .Route = route});
  if (!result.has_value()) {
    BOOST_LOG_TRIVIAL(warning) << "WinDivert: failed to inject packet";
  }
}

} // namespace gh
