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
  EndpointWinDivert(boost::asio::any_io_executor executor, std::string const& name);
  ~EndpointWinDivert() override;

  EndpointWinDivert(const EndpointWinDivert&) = delete;
  EndpointWinDivert& operator=(const EndpointWinDivert&) = delete;
  EndpointWinDivert(EndpointWinDivert&&) = delete;
  EndpointWinDivert& operator=(EndpointWinDivert&&) = delete;

  Omni::Fiber::Coroutine<ErrorCode> Read(Packet& p, Cancel&) override;
  Omni::Fiber::Coroutine<ErrorCode> Write(Packet& p, Cancel&) override;

protected:
  std::string GetName() const override;
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

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
