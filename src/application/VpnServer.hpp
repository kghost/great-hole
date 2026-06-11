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
  VpnServer& operator=(const VpnServer&) = delete;
  VpnServer(VpnServer&&) = delete;
  VpnServer& operator=(VpnServer&&) = delete;

  std::string GetName() const override;

  Omni::Fiber::Coroutine<void> RegisterPeer(const UdpDynMux::PskType& psk,
                                            const std::vector<boost::asio::ip::address_v6>& ips);
  Omni::Fiber::Coroutine<void> UnregisterPeer(const UdpDynMux::PskType& psk);

  Omni::Fiber::Coroutine<void> OnChannelEstablished(std::shared_ptr<UdpDynMux::Channel> channel) override;
  Omni::Fiber::Coroutine<void> OnChannelClosed(std::shared_ptr<UdpDynMux::Channel> channel) override;

protected:
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<void> DoWork() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  struct Session {
    std::shared_ptr<EndpointTunSplitIp::Channel> TunChannel;
    std::shared_ptr<Pipeline> Pipeline;
  };

  std::shared_ptr<EndpointTunSplitIp> _TunSplit;
  std::shared_ptr<UdpDynMux> _UdpDynMux;
  std::vector<std::shared_ptr<Filter>> _Filters;
  std::map<UdpDynMux::PskType, std::vector<boost::asio::ip::address_v6>> _Peers;
  std::map<std::shared_ptr<UdpDynMux::Channel>, Session> _Sessions;
  Omni::Fiber::RemoteCall _ChannelCall;
};

} // namespace gh
