#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/asio.hpp>

#include "ConnectionTracker.hpp"
#include "Coroutine.hpp"
#include "EndpointUdpDynMux.hpp"
#include "ErrorCode.hpp"
#include "Interface.hpp"
#include "VpnClientMultiChannel.hpp"

#ifdef _WIN32
#include "EndpointWinDivert.hpp"
#endif

namespace gh {

using DataPlaneCallbacks = Interface::DataPlaneCallbacks;

class TunnelDataPlane : public VpnClientMultiChannel::SessionStateListener {
public:
#ifdef _WIN32
  TunnelDataPlane(boost::asio::any_io_executor executor, ConnectionTracker::Selector& selector,
                  WinDivertRouteCallback& routeCallback, DataPlaneCallbacks& callbacks);
#else
  TunnelDataPlane(boost::asio::any_io_executor executor, ConnectionTracker::Selector& selector,
                  DataPlaneCallbacks& callbacks);
#endif
  ~TunnelDataPlane();

  void OnSessionStarting(const std::weak_ptr<VpnClientMultiChannelSession>& session) override;
  void OnSessionRunning(const std::weak_ptr<VpnClientMultiChannelSession>& session) override;
  void OnSessionStopping(const std::weak_ptr<VpnClientMultiChannelSession>& session) override;
  void OnSessionStopped(const std::weak_ptr<VpnClientMultiChannelSession>& session) override;
  void OnSessionFailed(const std::weak_ptr<VpnClientMultiChannelSession>& session, const std::string& error) override;

  TunnelDataPlane(const TunnelDataPlane&) = delete;
  auto operator=(const TunnelDataPlane&) -> TunnelDataPlane& = delete;
  TunnelDataPlane(TunnelDataPlane&&) = delete;
  auto operator=(TunnelDataPlane&&) -> TunnelDataPlane& = delete;

#ifdef _WIN32
  auto Start(int mtu, std::vector<char> encryptionKey) -> Omni::Fiber::Coroutine<ErrorCode>;
  [[nodiscard]] auto GetConnectionTracker() const -> const std::shared_ptr<ConnectionTracker>& {
    return _ConnectionTracker;
  }
  [[nodiscard]] auto GetInjector() const -> DeferredPacketInjector& { return *_WinDivert; }
#else
  auto Start(int tunFd, int mtu, std::vector<char> encryptionKey) -> Omni::Fiber::Coroutine<ErrorCode>;
  auto MigrateTun(int tunFd) -> Omni::Fiber::Coroutine<void>;
#endif
  auto Stop() -> Omni::Fiber::Coroutine<ErrorCode>;
  auto AddEndpoint(const UdpDynMux::PskType& psk, const std::string& address)
      -> Omni::Fiber::Coroutine<std::weak_ptr<VpnClientMultiChannelSession>>;
  auto RemoveEndpoint(std::weak_ptr<VpnClientMultiChannelSession> session) -> Omni::Fiber::Coroutine<void>;

  static auto GetTrafficStats(const std::shared_ptr<VpnClientMultiChannelSession>& session)
      -> std::optional<VpnTrafficStats>;

private:
  boost::asio::any_io_executor _Executor;
  ConnectionTracker::Selector& _Selector;
  DataPlaneCallbacks& _Callbacks;
#ifdef _WIN32
  std::shared_ptr<ConnectionTracker> _ConnectionTracker;
  std::shared_ptr<WinDivert> _WinDivert;
#endif
  std::shared_ptr<VpnClientMultiChannel> _Client;
  bool _Running = false;
};

} // namespace gh
