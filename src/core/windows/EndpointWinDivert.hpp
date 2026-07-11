#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <optional>
#include <string>
#include <windivert.h>
#include <windows.h>

#include "Endpoint.hpp"

namespace gh {

class WinDivertFastByPassCallback {
public:
  explicit WinDivertFastByPassCallback() = default;
  virtual ~WinDivertFastByPassCallback() = default;

  WinDivertFastByPassCallback(const WinDivertFastByPassCallback&) = delete;
  auto operator=(const WinDivertFastByPassCallback&) -> WinDivertFastByPassCallback& = delete;
  WinDivertFastByPassCallback(WinDivertFastByPassCallback&&) = delete;
  auto operator=(WinDivertFastByPassCallback&&) -> WinDivertFastByPassCallback& = delete;

  virtual auto WinDivertShouldByPass(Packet& packet, const WINDIVERT_ADDRESS& addr) -> bool = 0;
};

class WinDivert : public Endpoint {
public:
  WinDivert(boost::asio::any_io_executor executor, std::string name, WinDivertFastByPassCallback& callback);
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
  WinDivertFastByPassCallback& _FastByPassCallback;

  HANDLE _WinDivertHandle = INVALID_HANDLE_VALUE;
  HANDLE _ReadEvent = nullptr;
  HANDLE _WriteEvent = nullptr;

  std::optional<boost::asio::windows::object_handle> _ReadObjectHandle;
  std::optional<boost::asio::windows::object_handle> _WriteObjectHandle;

  std::atomic<uint32_t> _LastIfIdx{0};
  std::atomic<uint32_t> _LastSubIfIdx{0};
};

} // namespace gh
