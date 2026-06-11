#pragma once

#include <map>
#include <memory>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/ip/address_v6.hpp>

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "EndpointTunSplitIp.hpp"
#include "EndpointUdpDynMux.hpp"
#include "Filter.hpp"
#include "Pipeline.hpp"
#include "RemoteCall.hpp"

namespace gh {

class VpnServer : public UdpDynMux::ChannelNotification, public std::enable_shared_from_this<VpnServer> {
public:
  explicit VpnServer(std::shared_ptr<EndpointTunSplitIp> tunSplit, std::vector<std::shared_ptr<Filter>> filters = {});
  ~VpnServer() override;

  VpnServer(const VpnServer&) = delete;
  VpnServer& operator=(const VpnServer&) = delete;
  VpnServer(VpnServer&&) = delete;
  VpnServer& operator=(VpnServer&&) = delete;

  void RegisterPeer(const UdpDynMux::PskType& psk, const std::vector<boost::asio::ip::address_v6>& ips);
  void UnregisterPeer(const UdpDynMux::PskType& psk);

  Omni::Fiber::Coroutine<void> OnChannelEstablished(std::shared_ptr<UdpDynMux::Channel> channel) override;
  Omni::Fiber::Coroutine<void> OnChannelClosed(std::shared_ptr<UdpDynMux::Channel> channel) override;

  Omni::Fiber::Coroutine<void> Run(Cancel& c);

private:
  struct Session {
    std::shared_ptr<EndpointTunSplitIp::Channel> TunChannel;
    std::shared_ptr<Pipeline> Pipeline;
  };

  std::shared_ptr<EndpointTunSplitIp> _TunSplit;
  std::vector<std::shared_ptr<Filter>> _Filters;
  std::map<UdpDynMux::PskType, std::vector<boost::asio::ip::address_v6>> _Peers;
  std::map<std::shared_ptr<UdpDynMux::Channel>, Session> _Sessions;
  Omni::Fiber::RemoteCall _ChannelCall;
};

} // namespace gh
