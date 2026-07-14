#pragma once

#include <boost/asio.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <windivert.h>
#include <windows.h>

#include "DeferredPacketInjector.hpp"
#include "Endpoint.hpp"
#include "Pipe.hpp"

namespace gh {

class WinDivertRouteCallback {
public:
  explicit WinDivertRouteCallback() = default;
  virtual ~WinDivertRouteCallback() = default;

  WinDivertRouteCallback(const WinDivertRouteCallback&) = delete;
  auto operator=(const WinDivertRouteCallback&) -> WinDivertRouteCallback& = delete;
  WinDivertRouteCallback(WinDivertRouteCallback&&) = delete;
  auto operator=(WinDivertRouteCallback&&) -> WinDivertRouteCallback& = delete;

  enum class Result : std::uint8_t { Bypass, Discard, Normal };
  virtual auto WinDivertRoute(Packet& packet, const WINDIVERT_ADDRESS& addr) -> Result = 0;
};

class WinDivert : public Endpoint, public DeferredPacketInjector {
public:
  WinDivert(boost::asio::any_io_executor executor, std::string name, uint32_t ifIdx, uint32_t ifSubIdx,
            WinDivertRouteCallback& callback);
  ~WinDivert() override;

  WinDivert(const WinDivert&) = delete;
  auto operator=(const WinDivert&) -> WinDivert& = delete;
  WinDivert(WinDivert&&) = delete;
  auto operator=(WinDivert&&) -> WinDivert& = delete;

  auto Read(Packet& packet, Cancel& cancel) -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto Write(Packet& packet, Cancel& cancel) -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto Inject(Packet&& packet) -> Omni::Fiber::Coroutine<void> override;

protected:
  auto GetName() const -> std::string override;
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  boost::asio::any_io_executor _Executor;
  const std::string _Name;
  const uint32_t _IfIdx;
  const uint32_t _IfSubIdx;
  WinDivertRouteCallback& _RouteCallback;
  Omni::Fiber::Pipe<Packet> _InjectedPacketPipe;

  HANDLE _WinDivertHandle = INVALID_HANDLE_VALUE;
  HANDLE _ReadEvent = nullptr;
  HANDLE _WriteEvent = nullptr;

  std::optional<boost::asio::windows::object_handle> _ReadObjectHandle;
  std::optional<boost::asio::windows::object_handle> _WriteObjectHandle;
};

} // namespace gh
