#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <windows.h>

#include <boost/asio.hpp>

#include "Endpoint.hpp"

namespace gh {

class EndpointWinDivert : public Endpoint {
public:
  EndpointWinDivert(boost::asio::any_io_executor executor, std::string name);
  ~EndpointWinDivert() override;

  EndpointWinDivert(const EndpointWinDivert&) = delete;
  auto operator=(const EndpointWinDivert&) -> EndpointWinDivert& = delete;
  EndpointWinDivert(EndpointWinDivert&&) = delete;
  auto operator=(EndpointWinDivert&&) -> EndpointWinDivert& = delete;

  auto Read(Packet& packet, Cancel& cancel) -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto Write(Packet& packet, Cancel& cancel) -> Omni::Fiber::Coroutine<ErrorCode> override;

protected:
  auto GetName() const -> std::string override;
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  boost::asio::any_io_executor _Executor;
  const std::string _Name;

  HANDLE _WinDivertHandle;
  HANDLE _ReadEvent = nullptr;
  HANDLE _WriteEvent = nullptr;

  std::optional<boost::asio::windows::object_handle> _ReadObjectHandle;
  std::optional<boost::asio::windows::object_handle> _WriteObjectHandle;

  std::atomic<uint32_t> _LastIfIdx{0};
  std::atomic<uint32_t> _LastSubIfIdx{0};
};

} // namespace gh
