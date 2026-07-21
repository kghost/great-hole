#pragma once

#include <map>
#include <memory>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/ip/address_v6.hpp>

#include "Coroutine.hpp"
#include "EndpointTunSplitIp.hpp"
#include "EndpointUdpDynMux.hpp"
#include "Filter.hpp"
#include "Pipeline.hpp"
#include "RemoteCall.hpp"
#include "ServiceBase.hpp"

namespace gh {

class VpnServer : public ServiceBase, public UdpDynMux::ChannelNotification {
public:
  VpnServer(std::shared_ptr<EndpointTunSplitIp> tunSplit, std::shared_ptr<UdpDynMux> udpDynMux,
            std::vector<std::shared_ptr<Filter>> filters = {});
  ~VpnServer() override;

  VpnServer(const VpnServer&) = delete;
  auto operator=(const VpnServer&) -> VpnServer& = delete;
  VpnServer(VpnServer&&) = delete;
  auto operator=(VpnServer&&) -> VpnServer& = delete;

  auto GetName() const -> std::string override;

  auto RegisterPeer(const UdpDynMux::PskType& psk, const std::vector<boost::asio::ip::address_v6>& ips)
      -> Omni::Fiber::Coroutine<void>;
  auto UnregisterPeer(const UdpDynMux::PskType& psk) -> Omni::Fiber::Coroutine<void>;

protected:
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

  auto OnChannelEstablished(std::shared_ptr<UdpDynMux::Channel> channel) -> Omni::Fiber::Coroutine<void> override;
  auto OnChannelClosed(std::shared_ptr<UdpDynMux::Channel> channel) -> Omni::Fiber::Coroutine<void> override;

private:
  struct Session {
    std::shared_ptr<EndpointTunSplitIp::Channel> TunChannel;
    std::shared_ptr<Pipeline> SessionPipeline;
  };

  std::shared_ptr<EndpointTunSplitIp> _TunSplit;
  std::shared_ptr<UdpDynMux> _UdpDynMux;
  std::vector<std::shared_ptr<Filter>> _Filters;

  // TODO: merge following maps into multiply key to Session map.
  std::map<UdpDynMux::PskType, std::vector<boost::asio::ip::address_v6>> _Peers;
  std::map<UdpDynMux::PskType, std::shared_ptr<UdpDynMux::Channel>> _PeerChannels;
  std::map<std::shared_ptr<UdpDynMux::Channel>, Session> _Sessions;
  Omni::Fiber::RemoteCall _ChannelCall;
};

} // namespace gh
