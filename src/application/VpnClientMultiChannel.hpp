#pragma once

#include <boost/asio.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>

#include "ConnectionTracker.hpp"
#include "Coroutine.hpp"
#include "Endpoint.hpp"
#include "EndpointUdpDynMux.hpp"
#include "ErrorCode.hpp"
#include "Filter.hpp"
#include "Packet.hpp"
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
    [[nodiscard]] auto GetDescription() const -> std::string {
      return Channel ? Channel->GetName() : "Invalid Session";
    }

    std::shared_ptr<UdpDynMux::Channel> Channel;
    std::shared_ptr<ChannelSideEndpoint> ChannelSide;
    std::shared_ptr<Pipeline> SessionPipeline;
    bool Running = true;
  };

  class Mark : public ConnectionMark, public PacketMark {
  public:
    struct ToBeSelected {};
    struct Bypass {};
    struct Discard {};
    struct Deferred {
      std::vector<Packet> Packets;
    };

    using ValueType = std::variant<ToBeSelected, Bypass, Discard, Deferred, std::weak_ptr<Session>>;

    explicit Mark() : _Value(ToBeSelected{}) {}
    explicit Mark(Bypass /*unused*/) : _Value(Bypass{}) {}
    explicit Mark(Discard /*unused*/) : _Value(Discard{}) {}
    explicit Mark(Deferred deferred) : _Value(std::move(deferred)) {}
    explicit Mark(std::shared_ptr<Session> session) : _Value(std::move(session)) {}
    ~Mark() override = default;

    Mark(const Mark&) = delete;
    auto operator=(const Mark&) -> Mark& = delete;
    Mark(Mark&&) = delete;
    auto operator=(Mark&&) -> Mark& = delete;

    [[nodiscard]] auto GetDescription() const -> std::string override;
    [[nodiscard]] auto Validate() const -> bool override;
    [[nodiscard]] auto GetValue() const -> const ValueType& { return _Value; }

  private:
    ValueType _Value;
  };

  class SessionStateListener {
  public:
    explicit SessionStateListener() = default;
    virtual ~SessionStateListener() = default;

    SessionStateListener(const SessionStateListener&) = delete;
    auto operator=(const SessionStateListener&) -> SessionStateListener& = delete;
    SessionStateListener(SessionStateListener&&) = delete;
    auto operator=(SessionStateListener&&) -> SessionStateListener& = delete;

    virtual void OnSessionStarting(const std::shared_ptr<Session>& session) = 0;
    virtual void OnSessionRunning(const std::shared_ptr<Session>& session) = 0;
    virtual void OnSessionStopping(const std::shared_ptr<Session>& session) = 0;
    virtual void OnSessionStopped(const std::shared_ptr<Session>& session) = 0;
    virtual void OnSessionFailed(const std::shared_ptr<Session>& session, const std::string& error) = 0;
  };

  class NoopSessionStateListener : public SessionStateListener {
  public:
    explicit NoopSessionStateListener() = default;
    ~NoopSessionStateListener() override = default;

    NoopSessionStateListener(const NoopSessionStateListener&) = delete;
    auto operator=(const NoopSessionStateListener&) -> NoopSessionStateListener& = delete;
    NoopSessionStateListener(NoopSessionStateListener&&) = delete;
    auto operator=(NoopSessionStateListener&&) -> NoopSessionStateListener& = delete;

    void OnSessionStarting(const std::shared_ptr<Session>& /*session*/) override {}
    void OnSessionRunning(const std::shared_ptr<Session>& /*session*/) override {}
    void OnSessionStopping(const std::shared_ptr<Session>& /*session*/) override {}
    void OnSessionStopped(const std::shared_ptr<Session>& /*session*/) override {}
    void OnSessionFailed(const std::shared_ptr<Session>& /*session*/, const std::string& /*error*/) override {}
  };
  static NoopSessionStateListener _NoopSessionStateListener;

  VpnClientMultiChannel(boost::asio::any_io_executor executor, std::shared_ptr<Endpoint> tun,
                        std::shared_ptr<UdpDynMux> udpDynMux, std::shared_ptr<ConnectionTracker> tracker,
                        ConnectionTracker::Selector& selector, std::vector<std::shared_ptr<Filter>> filters,
                        SessionStateListener& listener = _NoopSessionStateListener);
  ~VpnClientMultiChannel() override;

  VpnClientMultiChannel(const VpnClientMultiChannel&) = delete;
  auto operator=(const VpnClientMultiChannel&) -> VpnClientMultiChannel& = delete;
  VpnClientMultiChannel(VpnClientMultiChannel&&) = delete;
  auto operator=(VpnClientMultiChannel&&) -> VpnClientMultiChannel& = delete;

  auto GetName() const -> std::string override;

  auto RegisterChannel(const UdpDynMux::PskType& psk, const std::string& address)
      -> Omni::Fiber::Coroutine<std::shared_ptr<Session>>;
  auto UnregisterChannel(std::shared_ptr<Session> session) -> Omni::Fiber::Coroutine<void>;
  auto MigrateTun(std::shared_ptr<Endpoint> newTun) -> Omni::Fiber::Coroutine<ErrorCode>;

  static auto GetStats(const std::shared_ptr<Session>& session) -> std::optional<VpnTrafficStats>;

  auto OnChannelEstablished(std::shared_ptr<UdpDynMux::Channel> channel) -> Omni::Fiber::Coroutine<void> override;
  auto OnChannelClosed(std::shared_ptr<UdpDynMux::Channel> channel) -> Omni::Fiber::Coroutine<void> override;

protected:
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

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
