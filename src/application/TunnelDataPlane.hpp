#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/address_v4.hpp>

#include "ConnectionTracker.hpp"
#include "Coroutine.hpp"
#include "EndpointUdpDynMux.hpp"
#include "ErrorCode.hpp"
#include "InterfaceCommonTypes.hpp"
#include "VpnClientMultiChannel.hpp"

#ifdef _WIN32
#include "EndpointWinDivert.hpp"
#include "InterfaceWin32.hpp"
#endif

namespace gh {

#ifdef _WIN32
class WindowsLocalAddressMonitor {
public:
  explicit WindowsLocalAddressMonitor() = default;
  virtual ~WindowsLocalAddressMonitor() = default;

  WindowsLocalAddressMonitor(const WindowsLocalAddressMonitor&) = delete;
  WindowsLocalAddressMonitor(WindowsLocalAddressMonitor&&) = delete;
  auto operator=(const WindowsLocalAddressMonitor&) -> WindowsLocalAddressMonitor& = delete;
  auto operator=(WindowsLocalAddressMonitor&&) -> WindowsLocalAddressMonitor& = delete;

  using Address = std::variant<boost::asio::ip::address_v4, boost::asio::ip::address_v6>;
  struct IpAddressInfo {
    uint8_t PrefixLength;
    WinDivertRouteCallback::InterfaceIndex InterfaceIndex;
  };

  [[nodiscard]] virtual auto GetAddressInfo(const Address& address) const -> std::optional<IpAddressInfo> = 0;
};
#endif

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

class TunnelDataPlane : public ConnectionTracker::Selector
#ifdef _WIN32
    ,
                        public WinDivertRouteCallback
#endif
{
public:
#ifdef _WIN32
  TunnelDataPlane(boost::asio::any_io_executor executor, TunnelDataPlanePolicyResolverCallback& policyResolver,
                  Interface::DataPlaneCallbacks& callbacks, std::span<Interface::IpAddress> addresses, int32_t mtu,
                  WindowsLocalAddressMonitor& windowsLocalAddressMonitor);
#else
  TunnelDataPlane(boost::asio::any_io_executor executor, TunnelDataPlanePolicyResolverCallback& policyResolver,
                  Interface::DataPlaneCallbacks& callbacks);
#endif
  ~TunnelDataPlane();

  TunnelDataPlane(const TunnelDataPlane&) = delete;
  auto operator=(const TunnelDataPlane&) -> TunnelDataPlane& = delete;
  TunnelDataPlane(TunnelDataPlane&&) = delete;
  auto operator=(TunnelDataPlane&&) -> TunnelDataPlane& = delete;

#ifdef _WIN32
  auto Start(std::vector<char> encryptionKey) -> Omni::Fiber::Coroutine<ErrorCode>;
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

  auto Select(const ConnectionTracker::ConnectionKey& key) -> ConnectionTracker::Selector::Action override;
#ifdef _WIN32
  auto WinDivertRouteOutbound(Packet& packet) -> WinDivertRouteCallback::Result override;
  auto WinDivertRouteInbound(Packet& packet) -> std::optional<uint32_t> override;
#endif

  [[nodiscard]] auto GetConnections() const -> std::vector<Interface::TrackedConnectionInfo>;
  static auto GetTrafficStats(const std::weak_ptr<VpnClientMultiChannelSession>& weak)
      -> std::optional<Interface::VpnTrafficStats>;

private:
  boost::asio::any_io_executor _Executor;
  TunnelDataPlanePolicyResolverCallback& _PolicyResolver;
  Interface::DataPlaneCallbacks& _Callbacks;
  std::shared_ptr<ConnectionTracker> _ConnectionTracker;
#ifdef _WIN32
  WindowsLocalAddressMonitor& _WindowsLocalAddressMonitor;
  struct NatContext {
    std::vector<boost::asio::ip::address_v4> _Ip4Addresses;
    std::vector<boost::asio::ip::address_v6> _Ip6Addresses;
    int32_t _Mtu;
  } _NatContext;
#endif
  std::shared_ptr<VpnClientMultiChannel> _Client;
  bool _Running = false;
};

} // namespace gh
