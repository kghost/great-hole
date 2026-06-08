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
#include "Pipe.hpp"
#include "Pipeline.hpp"

namespace gh {

class VpnServer : public UdpDynMux::ChannelNotification, public std::enable_shared_from_this<VpnServer> {
public:
  enum class EventType { kEstablished, kClosed };

  struct Event {
    EventType Type;
    std::shared_ptr<UdpDynMux::Channel> Channel;
  };

  explicit VpnServer(std::shared_ptr<EndpointTunSplitIp> tunSplit);
  ~VpnServer() override;

  VpnServer(const VpnServer&) = delete;
  VpnServer& operator=(const VpnServer&) = delete;
  VpnServer(VpnServer&&) = delete;
  VpnServer& operator=(VpnServer&&) = delete;

  void RegisterPeer(const UdpDynMux::PskType& psk, const std::vector<boost::asio::ip::address_v6>& ips);
  void UnregisterPeer(const UdpDynMux::PskType& psk);

  Omni::Fiber::Coroutine<void> OnChannelEstablished(std::shared_ptr<UdpDynMux::Channel> ch) override;
  Omni::Fiber::Coroutine<void> OnChannelClosed(std::shared_ptr<UdpDynMux::Channel> ch) override;

  Omni::Fiber::Coroutine<void> Run(Cancel& c);

private:
  struct Session {
    std::shared_ptr<EndpointTunSplitIp::Channel> TunChannel;
    std::shared_ptr<Pipeline> InPipeline;
    std::shared_ptr<Pipeline> OutPipeline;
  };

  std::shared_ptr<EndpointTunSplitIp> _TunSplit;
  std::map<UdpDynMux::PskType, std::vector<boost::asio::ip::address_v6>> _Peers;
  std::map<std::shared_ptr<UdpDynMux::Channel>, Session> _Sessions;
  Omni::Fiber::Pipe<Event> _Events;
};

} // namespace gh
