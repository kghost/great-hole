#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include <boost/asio.hpp>

#include "ConnectionTracker.hpp"
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
  class TunSideEndpoint;
  class ChannelSideEndpoint;

  class Session : public ConnectionMark {
  public:
    Session() {}
    std::string GetDescription() const { return Channel ? Channel->GetName() : "Invalid Session"; }

    std::shared_ptr<UdpDynMux::Channel> Channel;
    std::shared_ptr<ChannelSideEndpoint> ChannelSide;
    std::shared_ptr<Pipeline> SessionPipeline;
    bool Running = true;
  };

  VpnClientMultiChannel(boost::asio::any_io_executor executor, std::shared_ptr<Endpoint> tun,
                        std::shared_ptr<UdpDynMux> udpDynMux, ConnectionTracker::SelectorType selector,
                        std::vector<std::shared_ptr<Filter>> filters = {});
  ~VpnClientMultiChannel() override;

  VpnClientMultiChannel(const VpnClientMultiChannel&) = delete;
  VpnClientMultiChannel& operator=(const VpnClientMultiChannel&) = delete;
  VpnClientMultiChannel(VpnClientMultiChannel&&) = delete;
  VpnClientMultiChannel& operator=(VpnClientMultiChannel&&) = delete;

  void SetConntrackTimeoutForTesting(std::chrono::seconds timeout);

  std::string GetName() const override;

  Omni::Fiber::Coroutine<std::reference_wrapper<Session>> RegisterChannel(const UdpDynMux::PskType& psk,
                                                                          std::shared_ptr<ResolverEndpoint> resolver);
  Omni::Fiber::Coroutine<void> UnregisterChannel(Session& session);

  std::pair<uint64_t, uint64_t> GetStats(Session& session) const;

  Omni::Fiber::Coroutine<void> OnChannelEstablished(std::shared_ptr<UdpDynMux::Channel> channel) override;
  Omni::Fiber::Coroutine<void> OnChannelClosed(std::shared_ptr<UdpDynMux::Channel> channel) override;

protected:
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<void> DoWork() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  boost::asio::any_io_executor _Executor;
  std::shared_ptr<Endpoint> _Tun;
  std::shared_ptr<UdpDynMux> _UdpDynMux;
  ConnectionTracker::SelectorType _Selector;
  std::vector<std::shared_ptr<Filter>> _Filters;

  std::shared_ptr<TunSideEndpoint> _TunSide;
  std::shared_ptr<Pipeline> _TunPipeline;

  std::map<UdpDynMux::PskType, Session> _Sessions;
  Omni::Fiber::RemoteCall _ChannelCall;
};

} // namespace gh
