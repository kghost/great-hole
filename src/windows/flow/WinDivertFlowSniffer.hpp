#pragma once

#include <boost/asio.hpp>
#include <boost/asio/windows/object_handle.hpp>
#include <optional>
#include <windows.h>

#include "ConnectionTracker.hpp"
#include "ServiceBase.hpp"

namespace gh {

class WinDivertFlowSnifferCallback {
public:
  explicit WinDivertFlowSnifferCallback() = default;
  virtual ~WinDivertFlowSnifferCallback() = default;

  WinDivertFlowSnifferCallback(const WinDivertFlowSnifferCallback&) = delete;
  auto operator=(const WinDivertFlowSnifferCallback&) -> WinDivertFlowSnifferCallback& = delete;
  WinDivertFlowSnifferCallback(WinDivertFlowSnifferCallback&&) = delete;
  auto operator=(WinDivertFlowSnifferCallback&&) -> WinDivertFlowSnifferCallback& = delete;

  virtual auto OnFlowEstablished(const ConnectionTracker::ConnectionKey& conn, uint32_t pid)
      -> Omni::Fiber::Coroutine<void> = 0;
  virtual auto OnFlowDeleted(const ConnectionTracker::ConnectionKey& conn) -> Omni::Fiber::Coroutine<void> = 0;
};

class WinDivertFlowSniffer : public ServiceBase {
public:
  explicit WinDivertFlowSniffer(boost::asio::any_io_executor executor, WinDivertFlowSnifferCallback& callback);
  ~WinDivertFlowSniffer() override;

  WinDivertFlowSniffer(const WinDivertFlowSniffer&) = delete;
  auto operator=(const WinDivertFlowSniffer&) -> WinDivertFlowSniffer& = delete;
  WinDivertFlowSniffer(WinDivertFlowSniffer&&) = delete;
  auto operator=(WinDivertFlowSniffer&&) -> WinDivertFlowSniffer& = delete;

  auto GetName() const -> std::string override { return "WinDivertFlowSniffer"; }

protected:
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  boost::asio::any_io_executor _Executor;
  WinDivertFlowSnifferCallback& _Callback;
  HANDLE _WinDivertFlowHandle = INVALID_HANDLE_VALUE;
  HANDLE _ReadEvent = nullptr;
  std::optional<boost::asio::windows::object_handle> _ReadObject;
  OVERLAPPED _Overlapped{};
};

} // namespace gh
