#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <boost/asio.hpp>

#include <windivert.h>
#include <windows.h>

#include "Endpoint.hpp"

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
  using InterfaceIndex = uint32_t;
  virtual auto WinDivertRouteOutbound(Packet& packet) -> Result = 0;
  virtual auto WinDivertRouteInbound(Packet& packet) -> std::optional<InterfaceIndex> = 0;
};

class WinDivert : public Endpoint {
public:
  WinDivert(boost::asio::any_io_executor executor, std::string name, WinDivertRouteCallback& callback);
  ~WinDivert() override;

  WinDivert(const WinDivert&) = delete;
  auto operator=(const WinDivert&) -> WinDivert& = delete;
  WinDivert(WinDivert&&) = delete;
  auto operator=(WinDivert&&) -> WinDivert& = delete;

  auto Read(Packet& packet, Cancel& cancel) -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto Write(Packet& packet, Cancel& cancel) -> Omni::Fiber::Coroutine<ErrorCode> override;

protected:
  auto GetName() const -> std::string override;
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  boost::asio::any_io_executor _Executor;
  const std::string _Name;
  WinDivertRouteCallback& _RouteCallback;

  HANDLE _WinDivertHandle = INVALID_HANDLE_VALUE;
  HANDLE _ReadEvent = nullptr;
  HANDLE _WriteEvent = nullptr;

  std::optional<boost::asio::windows::object_handle> _ReadObjectHandle;
  std::optional<boost::asio::windows::object_handle> _WriteObjectHandle;
};

} // namespace gh
