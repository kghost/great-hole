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

class TunnelDataPlanePolicyResolverCallback {
public:
  explicit TunnelDataPlanePolicyResolverCallback() = default;
  virtual ~TunnelDataPlanePolicyResolverCallback() = default;

  TunnelDataPlanePolicyResolverCallback(const TunnelDataPlanePolicyResolverCallback&) = default;
  TunnelDataPlanePolicyResolverCallback(TunnelDataPlanePolicyResolverCallback&&) = delete;
  auto operator=(const TunnelDataPlanePolicyResolverCallback&) -> TunnelDataPlanePolicyResolverCallback& = default;
  auto operator=(TunnelDataPlanePolicyResolverCallback&&) -> TunnelDataPlanePolicyResolverCallback& = delete;

  [[nodiscard]] virtual auto ResolvePolicy(const ConnectionTracker::ConnectionKey& key)
      -> Interface::PolicyRule::RoutingAction = 0;
};

class TunnelDataPlane : public VpnClientMultiChannel::SessionStateListener,
                        public ConnectionTracker::Selector,
#ifdef _WIN32
                        public WinDivertRouteCallback
#endif
{
public:
  TunnelDataPlane(boost::asio::any_io_executor executor, TunnelDataPlanePolicyResolverCallback& policyResolver,
                  Interface::DataPlaneCallbacks& callbacks);
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
#else
  auto Start(int tunFd, int mtu, std::vector<char> encryptionKey) -> Omni::Fiber::Coroutine<ErrorCode>;
  auto MigrateTun(int tunFd) -> Omni::Fiber::Coroutine<void>;
#endif
  auto Stop() -> Omni::Fiber::Coroutine<ErrorCode>;
  auto AddEndpoint(const UdpDynMux::PskType& psk, const std::string& address)
      -> std::weak_ptr<VpnClientMultiChannelSession>;
  void RemoveEndpoint(const std::weak_ptr<VpnClientMultiChannelSession>& weak);

  auto StartEndpoint(const std::weak_ptr<VpnClientMultiChannelSession>& weak) -> Omni::Fiber::Coroutine<void>;
  auto StopEndpoint(const std::weak_ptr<VpnClientMultiChannelSession>& weak) -> Omni::Fiber::Coroutine<void>;

  auto SelectConnectionMark(const ConnectionTracker::ConnectionKey& key) -> std::shared_ptr<ConnectionMark> override;
  auto WinDivertRoute(Packet& packet, const WINDIVERT_ADDRESS& addr) -> WinDivertRouteCallback::Result override;

  [[nodiscard]] auto GetConnections() const -> std::vector<Interface::TrackedConnectionInfo>;
  static auto GetTrafficStats(const std::shared_ptr<VpnClientMultiChannelSession>& session)
      -> std::optional<VpnTrafficStats>;

private:
  boost::asio::any_io_executor _Executor;
  TunnelDataPlanePolicyResolverCallback& _PolicyResolver;
  Interface::DataPlaneCallbacks& _Callbacks;
  std::shared_ptr<ConnectionTracker> _ConnectionTracker;
  std::shared_ptr<VpnClientMultiChannel> _Client;
  bool _Running = false;
};

} // namespace gh
