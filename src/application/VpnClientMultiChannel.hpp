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
#include "Pipeline.hpp"
#include "RemoteCall.hpp"
#include "ServiceBase.hpp"

namespace gh {

struct VpnTrafficStats : public TrafficStats {
  VpnTrafficStats() = default;
  explicit VpnTrafficStats(const TrafficStats& stats, int64_t rttMs = -1) : TrafficStats(stats), RttMs(rttMs) {}

  int64_t RttMs{-1};
};

class VpnClientMultiChannel : public ServiceBase, public UdpDynMux::ChannelNotification {
public:
  class TunSideEndpoint;
  class ChannelSideEndpoint;

  class Session {
  public:
    explicit Session() = default;
    std::string GetDescription() const { return Channel ? Channel->GetName() : "Invalid Session"; }

    std::shared_ptr<UdpDynMux::Channel> Channel;
    std::shared_ptr<ChannelSideEndpoint> ChannelSide;
    std::shared_ptr<Pipeline> SessionPipeline;
    bool Running = true;
  };

  class Mark : public ConnectionMark {
  public:
    struct ToBeSelected {};
    struct Bypass {};
    struct Discard {};

    using ValueType = std::variant<ToBeSelected, Bypass, Discard, std::weak_ptr<Session>>;

    explicit Mark() : _Value(ToBeSelected{}) {}
    explicit Mark(Bypass) : _Value(Bypass{}) {}
    explicit Mark(Discard) : _Value(Discard{}) {}
    explicit Mark(std::shared_ptr<Session> session) : _Value(std::move(session)) {}
    ~Mark() override = default;

    Mark(const Mark&) = delete;
    Mark(Mark&&) = delete;
    Mark& operator=(const Mark&) = delete;
    Mark& operator=(Mark&&) = delete;

    std::string GetDescription() const override;
    bool Validate() const override;

    const ValueType& GetValue() const { return _Value; }

  private:
    ValueType _Value;
  };

  class SessionStateListener {
  public:
    virtual ~SessionStateListener() = default;
    virtual void OnSessionStarting(std::shared_ptr<Session> session) = 0;
    virtual void OnSessionRunning(std::shared_ptr<Session> session) = 0;
    virtual void OnSessionStopping(std::shared_ptr<Session> session) = 0;
    virtual void OnSessionStopped(std::shared_ptr<Session> session) = 0;
    virtual void OnSessionFailed(std::shared_ptr<Session> session, const std::string& error) = 0;
  };

  class NoopSessionStateListener : public SessionStateListener {
  public:
    ~NoopSessionStateListener() override = default;
    void OnSessionStarting(std::shared_ptr<Session>) override {}
    void OnSessionRunning(std::shared_ptr<Session>) override {}
    void OnSessionStopping(std::shared_ptr<Session>) override {}
    void OnSessionStopped(std::shared_ptr<Session>) override {}
    void OnSessionFailed(std::shared_ptr<Session>, const std::string&) override {}
  };
  static NoopSessionStateListener _NoopSessionStateListener;

  VpnClientMultiChannel(boost::asio::any_io_executor executor, std::shared_ptr<Endpoint> tun,
                        std::shared_ptr<UdpDynMux> udpDynMux, std::shared_ptr<ConnectionTracker> tracker,
                        ConnectionTracker::Selector& selector,
                        std::vector<std::shared_ptr<Filter>> filters,
                        SessionStateListener& listener = _NoopSessionStateListener);
  ~VpnClientMultiChannel() override;

  VpnClientMultiChannel(const VpnClientMultiChannel&) = delete;
  VpnClientMultiChannel& operator=(const VpnClientMultiChannel&) = delete;
  VpnClientMultiChannel(VpnClientMultiChannel&&) = delete;
  VpnClientMultiChannel& operator=(VpnClientMultiChannel&&) = delete;

  std::string GetName() const override;

  Omni::Fiber::Coroutine<std::shared_ptr<Session>> RegisterChannel(const UdpDynMux::PskType& psk,
                                                                   std::shared_ptr<ResolverEndpoint> resolver);
  Omni::Fiber::Coroutine<void> UnregisterChannel(std::shared_ptr<Session> session);
  Omni::Fiber::Coroutine<ErrorCode> MigrateTun(std::shared_ptr<Endpoint> newTun);

  std::optional<VpnTrafficStats> GetStats(std::shared_ptr<Session> session) const;

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
  std::shared_ptr<ConnectionTracker> _ConnectionTracker;
  std::vector<std::shared_ptr<Filter>> _Filters;

  std::shared_ptr<TunSideEndpoint> _TunSide;
  std::shared_ptr<Pipeline> _TunPipeline;

  std::map<UdpDynMux::PskType, std::shared_ptr<Session>> _Sessions;
  Omni::Fiber::RemoteCall _ChannelCall;
  std::reference_wrapper<SessionStateListener> _StateListener;
};

} // namespace gh
