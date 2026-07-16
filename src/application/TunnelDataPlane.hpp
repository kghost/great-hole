#pragma once

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <boost/asio.hpp>

#include "ConnectionTracker.hpp"
#include "Coroutine.hpp"
#include "EndpointUdpDynMux.hpp"
#include "ErrorCode.hpp"
#include "Interface.hpp"
#include "Utils/Comparator.hpp"
#include "VpnClientMultiChannel.hpp"

#ifdef _WIN32
#include "DeferredPacketInjector.hpp"
#include "EndpointWinDivert.hpp"

#endif

namespace gh {

using DataPlaneCallbacks = Interface::DataPlaneCallbacks;

class TunnelDataPlane :
#ifdef _WIN32
    public WinDivertRouteCallback,
#endif
    public VpnClientMultiChannel::SessionStateListener {
public:
  TunnelDataPlane(boost::asio::any_io_executor executor, ConnectionTracker::Selector& selector,
                  DataPlaneCallbacks& callbacks);
  ~TunnelDataPlane();

  void OnSessionStarting(const std::shared_ptr<VpnClientMultiChannelSession>& session) override;
  void OnSessionRunning(const std::shared_ptr<VpnClientMultiChannelSession>& session) override;
  void OnSessionStopping(const std::shared_ptr<VpnClientMultiChannelSession>& session) override;
  void OnSessionStopped(const std::shared_ptr<VpnClientMultiChannelSession>& session) override;
  void OnSessionFailed(const std::shared_ptr<VpnClientMultiChannelSession>& session, const std::string& error) override;

  TunnelDataPlane(const TunnelDataPlane&) = delete;
  auto operator=(const TunnelDataPlane&) -> TunnelDataPlane& = delete;
  TunnelDataPlane(TunnelDataPlane&&) = delete;
  auto operator=(TunnelDataPlane&&) -> TunnelDataPlane& = delete;

#ifdef _WIN32
  auto Start(int mtu, std::vector<char> encryptionKey) -> Omni::Fiber::Coroutine<ErrorCode>;
  auto WinDivertRoute(Packet& packet, const WINDIVERT_ADDRESS& addr) -> WinDivertRouteCallback::Result override;
  [[nodiscard]] auto GetInjector() const -> DeferredPacketInjector& { return *_WinDivert; }
#else
  auto Start(int tunFd, int mtu, std::vector<char> encryptionKey) -> Omni::Fiber::Coroutine<void>;
  auto MigrateTun(int tunFd) -> Omni::Fiber::Coroutine<void>;
#endif
  auto Stop() -> Omni::Fiber::Coroutine<ErrorCode>;
  auto AddEndpoint(const UdpDynMux::PskType& psk, const std::string& address)
      -> Omni::Fiber::Coroutine<std::shared_ptr<VpnClientMultiChannelSession>>;
  auto RemoveEndpoint(std::shared_ptr<VpnClientMultiChannelSession> session) -> Omni::Fiber::Coroutine<void>;

  // TODO: remove this function
  auto FindSessionByHandle(VpnClientMultiChannelSession* session) -> std::shared_ptr<VpnClientMultiChannelSession>;

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

  std::set<std::shared_ptr<VpnClientMultiChannelSession>, SharedPtrCompare> _Endpoints;
  bool _Running = false;
};

} // namespace gh
