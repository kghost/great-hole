#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "Endpoint.hpp"
#include "EndpointUdpDynMux.hpp"
#include "ErrorCode.hpp"
#include "EventQueue.hpp"
#include "Filter.hpp"
#include "Packet.hpp"
#include "Pipeline.hpp"
#include "RemoteCall.hpp"
#include "ServiceBase.hpp"

namespace gh {

struct ConnectionKey {
  boost::asio::ip::address_v6 Addr1;
  boost::asio::ip::address_v6 Addr2;
  uint16_t Port1 = 0;
  uint16_t Port2 = 0;
  uint8_t Protocol = 0;

  bool operator==(const ConnectionKey& other) const {
    return Addr1 == other.Addr1 && Addr2 == other.Addr2 && Port1 == other.Port1 && Port2 == other.Port2 &&
           Protocol == other.Protocol;
  }

  bool operator<(const ConnectionKey& other) const {
    if (Addr1 != other.Addr1) {
      return Addr1 < other.Addr1;
    }
    if (Addr2 != other.Addr2) {
      return Addr2 < other.Addr2;
    }
    if (Port1 != other.Port1) {
      return Port1 < other.Port1;
    }
    if (Port2 != other.Port2) {
      return Port2 < other.Port2;
    }
    return Protocol < other.Protocol;
  }
};

class VpnConnTrack : public ServiceBase, public UdpDynMux::ChannelNotification {
public:
  using PeerSelector = std::function<std::shared_ptr<UdpDynMux::Channel>(
      const boost::asio::ip::address_v6& src, const boost::asio::ip::address_v6& dst, uint16_t srcPort,
      uint16_t dstPort, uint8_t protocol)>;

  VpnConnTrack(boost::asio::io_context& ioContext, std::shared_ptr<Endpoint> tun, std::shared_ptr<UdpDynMux> udpDynMux,
               PeerSelector selector, std::vector<std::shared_ptr<Filter>> filters = {});
  ~VpnConnTrack() override;

  VpnConnTrack(const VpnConnTrack&) = delete;
  VpnConnTrack& operator=(const VpnConnTrack&) = delete;
  VpnConnTrack(VpnConnTrack&&) = delete;
  VpnConnTrack& operator=(VpnConnTrack&&) = delete;

  class TunSideEndpoint;
  class ChannelSideEndpoint;

  struct Session {
    std::shared_ptr<ChannelSideEndpoint> ChannelSide;
    std::shared_ptr<Pipeline> Pipeline;
  };

  void SetConntrackTimeoutForTesting(std::chrono::seconds timeout) { _ConntrackTimeout = timeout; }

  std::string GetName() const override;

  Omni::Fiber::Coroutine<void> OnChannelEstablished(std::shared_ptr<UdpDynMux::Channel> channel) override;
  Omni::Fiber::Coroutine<void> OnChannelClosed(std::shared_ptr<UdpDynMux::Channel> channel) override;

  void UpdateConntrack(const ConnectionKey& key, std::shared_ptr<UdpDynMux::Channel> channel);

protected:
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<void> DoWork() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  struct ConnectionEntry {
    std::shared_ptr<UdpDynMux::Channel> Channel;
    std::chrono::steady_clock::time_point LastActive;
  };

  Omni::Fiber::Coroutine<void> PruneLoop();

  boost::asio::io_context& _IoContext;
  std::shared_ptr<Endpoint> _Tun;
  std::shared_ptr<UdpDynMux> _UdpDynMux;
  PeerSelector _Selector;
  std::vector<std::shared_ptr<Filter>> _Filters;
  std::chrono::seconds _ConntrackTimeout = std::chrono::seconds(60);

  std::shared_ptr<TunSideEndpoint> _TunSide;
  std::shared_ptr<Pipeline> _TunPipeline;
  Omni::Fiber::EventQueue<Packet> _Queue;

  std::map<ConnectionKey, ConnectionEntry> _ConntrackTable;
  std::set<std::shared_ptr<UdpDynMux::Channel>> _EstablishedChannels;
  std::map<std::shared_ptr<UdpDynMux::Channel>, Session> _Sessions;
  Omni::Fiber::RemoteCall _ChannelCall;
};

} // namespace gh
