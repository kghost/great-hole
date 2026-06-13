#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "Endpoint.hpp"
#include "EndpointUdpDynMux.hpp"
#include "ErrorCode.hpp"
#include "Filter.hpp"
#include "Pipeline.hpp"
#include "RemoteCall.hpp"
#include "ServiceBase.hpp"

namespace gh {

class VpnClientMultiChannel : public ServiceBase, public UdpDynMux::ChannelNotification {
public:
  using PeerSelector = std::function<std::shared_ptr<UdpDynMux::Channel>(
      const boost::asio::ip::address_v6& src, const boost::asio::ip::address_v6& dst, uint16_t srcPort,
      uint16_t dstPort, uint8_t protocol)>;

  VpnClientMultiChannel(boost::asio::io_context& ioContext, std::shared_ptr<Endpoint> tun,
                        std::shared_ptr<UdpDynMux> udpDynMux, PeerSelector selector,
                        std::vector<std::shared_ptr<Filter>> filters = {});
  ~VpnClientMultiChannel() override;

  VpnClientMultiChannel(const VpnClientMultiChannel&) = delete;
  VpnClientMultiChannel& operator=(const VpnClientMultiChannel&) = delete;
  VpnClientMultiChannel(VpnClientMultiChannel&&) = delete;
  VpnClientMultiChannel& operator=(VpnClientMultiChannel&&) = delete;

  class TunSideEndpoint;
  class ChannelSideEndpoint;

  struct Session {
    std::shared_ptr<ChannelSideEndpoint> ChannelSide;
    std::shared_ptr<Pipeline> Pipeline;
  };

  void SetConntrackTimeoutForTesting(std::chrono::seconds timeout);

  std::string GetName() const override;

  Omni::Fiber::Coroutine<void> OnChannelEstablished(std::shared_ptr<UdpDynMux::Channel> channel) override;
  Omni::Fiber::Coroutine<void> OnChannelClosed(std::shared_ptr<UdpDynMux::Channel> channel) override;

protected:
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<void> DoWork() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  Omni::Fiber::Coroutine<void> PruneLoop();

  boost::asio::io_context& _IoContext;
  std::shared_ptr<Endpoint> _Tun;
  std::shared_ptr<UdpDynMux> _UdpDynMux;
  PeerSelector _Selector;
  std::vector<std::shared_ptr<Filter>> _Filters;

  std::shared_ptr<TunSideEndpoint> _TunSide;
  std::shared_ptr<Pipeline> _TunPipeline;

  std::map<std::shared_ptr<UdpDynMux::Channel>, Session> _Sessions;
  Omni::Fiber::RemoteCall _ChannelCall;
};

} // namespace gh
