#pragma once

#include <boost/asio/any_io_executor.hpp>
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
#include "GHApi.hpp"
#include "Pipeline.hpp"
#include "RemoteCall.hpp"
#include "ServiceBase.hpp"

namespace gh {

struct VpnTrafficStats : public TrafficStats {
  VpnTrafficStats() = default;
  explicit VpnTrafficStats(const TrafficStats& stats, int64_t rttMs = -1) : TrafficStats(stats), RttMs(rttMs) {}

  int64_t RttMs{-1};
};

class GH_API VpnClientMultiChannel : public ServiceBase, public UdpDynMux::ChannelNotification {
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

  class SessionStateListener {
  public:
    virtual ~SessionStateListener() = default;
    virtual void OnSessionStarting(Session& session) = 0;
    virtual void OnSessionRunning(Session& session) = 0;
    virtual void OnSessionStopping(Session& session) = 0;
    virtual void OnSessionStopped(Session& session) = 0;
    virtual void OnSessionFailed(Session& session, const std::string& error) = 0;
  };

  class NoopSessionStateListener : public SessionStateListener {
  public:
    ~NoopSessionStateListener() override = default;
    void OnSessionStarting(Session&) override {}
    void OnSessionRunning(Session&) override {}
    void OnSessionStopping(Session&) override {}
    void OnSessionStopped(Session&) override {}
    void OnSessionFailed(Session&, const std::string&) override {}
  };
  static NoopSessionStateListener _NoopSessionStateListener;

  VpnClientMultiChannel(boost::asio::any_io_executor executor, std::shared_ptr<Endpoint> tun,
                        std::shared_ptr<UdpDynMux> udpDynMux, std::shared_ptr<ConnectionTracker> tracker,
                        std::vector<std::shared_ptr<Filter>> filters,
                        SessionStateListener& listener = _NoopSessionStateListener);
  ~VpnClientMultiChannel() override;

  VpnClientMultiChannel(const VpnClientMultiChannel&) = delete;
  VpnClientMultiChannel& operator=(const VpnClientMultiChannel&) = delete;
  VpnClientMultiChannel(VpnClientMultiChannel&&) = delete;
  VpnClientMultiChannel& operator=(VpnClientMultiChannel&&) = delete;

  std::string GetName() const override;

  Omni::Fiber::Coroutine<std::reference_wrapper<Session>> RegisterChannel(const UdpDynMux::PskType& psk,
                                                                          std::shared_ptr<ResolverEndpoint> resolver);
  Omni::Fiber::Coroutine<void> UnregisterChannel(Session& session);
  Omni::Fiber::Coroutine<ErrorCode> MigrateTun(std::shared_ptr<Endpoint> newTun);

  std::optional<VpnTrafficStats> GetStats(Session& session) const;

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
  std::vector<std::shared_ptr<Filter>> _Filters;

  std::shared_ptr<TunSideEndpoint> _TunSide;
  std::shared_ptr<Pipeline> _TunPipeline;

  std::map<UdpDynMux::PskType, Session> _Sessions;
  Omni::Fiber::RemoteCall _ChannelCall;
  std::reference_wrapper<SessionStateListener> _StateListener;
};

} // namespace gh
