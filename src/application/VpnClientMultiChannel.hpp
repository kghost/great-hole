#pragma once

#include <boost/asio.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <variant>

#include "ConnectionTracker.hpp"
#include "Coroutine.hpp"
#include "Endpoint.hpp"
#include "EndpointUdpDynMux.hpp"
#include "ErrorCode.hpp"
#include "Filter.hpp"
#include "Interface.hpp"
#include "Packet.hpp"
#include "Pipeline.hpp"
#include "RemoteCall.hpp"
#include "ServiceBase.hpp"

namespace gh {

using VpnTrafficStats = Interface::VpnTrafficStats;

class VpnClientMultiChannelSession;

class VpnClientMultiChannel : public ServiceBase, public UdpDynMux::ChannelNotification {
public:
  class TunSideEndpoint;
  class ChannelSideEndpoint;

  class Mark : public ConnectionMark, public PacketMark {
  public:
    struct ToBeSelected {};
    struct Bypass {};
    struct Discard {};
    struct Deferred {
      struct DeferredPacket {
        explicit DeferredPacket() = default;
        virtual ~DeferredPacket() = default;

        DeferredPacket(const DeferredPacket&) = default;
        auto operator=(const DeferredPacket&) -> DeferredPacket& = default;
        DeferredPacket(DeferredPacket&&) = delete;
        auto operator=(DeferredPacket&&) -> DeferredPacket& = delete;
      };
      std::vector<std::unique_ptr<DeferredPacket>> Packets;
    };

    using ValueType =
        std::variant<ToBeSelected, Bypass, Discard, Deferred, std::weak_ptr<VpnClientMultiChannelSession>>;

    explicit Mark() : _Value(ToBeSelected{}) {}
    explicit Mark(Bypass /*unused*/) : _Value(Bypass{}) {}
    explicit Mark(Discard /*unused*/) : _Value(Discard{}) {}
    explicit Mark(Deferred deferred) : _Value(std::move(deferred)) {}
    explicit Mark(std::weak_ptr<VpnClientMultiChannelSession> session) : _Value(std::move(session)) {}
    ~Mark() override = default;

    Mark(const Mark&) = delete;
    auto operator=(const Mark&) -> Mark& = delete;
    Mark(Mark&&) = delete;
    auto operator=(Mark&&) -> Mark& = delete;

    [[nodiscard]] auto GetDescription() const -> std::string override;
    [[nodiscard]] auto Validate() const -> bool override;
    [[nodiscard]] auto GetValue() -> ValueType& { return _Value; }
    void Swap(Mark& other) { _Value.swap(other._Value); }

    [[nodiscard]] auto GetPendingQueueSize() const -> std::optional<size_t> {
      if (const auto* const deferred = std::get_if<Deferred>(&_Value)) {
        return deferred->Packets.size();
      }
      return std::nullopt;
    }

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

    virtual void OnSessionStarting(const std::weak_ptr<VpnClientMultiChannelSession>& session) = 0;
    virtual void OnSessionRunning(const std::weak_ptr<VpnClientMultiChannelSession>& session) = 0;
    virtual void OnSessionStopping(const std::weak_ptr<VpnClientMultiChannelSession>& session) = 0;
    virtual void OnSessionStopped(const std::weak_ptr<VpnClientMultiChannelSession>& session) = 0;
    virtual void OnSessionFailed(const std::weak_ptr<VpnClientMultiChannelSession>& session,
                                 const std::string& error) = 0;
  };

  class NoopSessionStateListener : public SessionStateListener {
  public:
    explicit NoopSessionStateListener() = default;
    ~NoopSessionStateListener() override = default;

    NoopSessionStateListener(const NoopSessionStateListener&) = delete;
    auto operator=(const NoopSessionStateListener&) -> NoopSessionStateListener& = delete;
    NoopSessionStateListener(NoopSessionStateListener&&) = delete;
    auto operator=(NoopSessionStateListener&&) -> NoopSessionStateListener& = delete;

    void OnSessionStarting(const std::weak_ptr<VpnClientMultiChannelSession>& /*session*/) override {}
    void OnSessionRunning(const std::weak_ptr<VpnClientMultiChannelSession>& /*session*/) override {}
    void OnSessionStopping(const std::weak_ptr<VpnClientMultiChannelSession>& /*session*/) override {}
    void OnSessionStopped(const std::weak_ptr<VpnClientMultiChannelSession>& /*session*/) override {}
    void OnSessionFailed(const std::weak_ptr<VpnClientMultiChannelSession>& /*session*/,
                         const std::string& /*error*/) override {}
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
      -> std::weak_ptr<VpnClientMultiChannelSession>;
  void UnregisterChannel(const std::weak_ptr<VpnClientMultiChannelSession>& weak);

  auto StartChannel(const std::weak_ptr<VpnClientMultiChannelSession>& weak) -> Omni::Fiber::Coroutine<void>;
  auto StopChannel(const std::weak_ptr<VpnClientMultiChannelSession>& weak) -> Omni::Fiber::Coroutine<void>;

  auto MigrateTun(std::shared_ptr<Endpoint> newTun) -> Omni::Fiber::Coroutine<ErrorCode>;

  static auto GetStats(const std::weak_ptr<VpnClientMultiChannelSession>& weak) -> std::optional<VpnTrafficStats>;

  auto OnChannelEstablished(UdpDynMux::ChannelNotificationTarget& target) -> Omni::Fiber::Coroutine<void> override;
  auto OnChannelClosed(UdpDynMux::ChannelNotificationTarget& target) -> Omni::Fiber::Coroutine<void> override;

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

  std::set<std::shared_ptr<VpnClientMultiChannelSession>, std::owner_less<>> _Sessions;
  Omni::Fiber::RemoteCall _ChannelCall;
  std::reference_wrapper<SessionStateListener> _StateListener;
};

class VpnClientMultiChannelSession : public UdpDynMux::ChannelNotificationTarget,
                                     public std::enable_shared_from_this<VpnClientMultiChannelSession> {
public:
  explicit VpnClientMultiChannelSession(UdpDynMux::PskType psk, std::string address)
      : psk(psk), address(std::move(address)) {}
  ~VpnClientMultiChannelSession() override = default;

  VpnClientMultiChannelSession(const VpnClientMultiChannelSession&) = delete;
  auto operator=(const VpnClientMultiChannelSession&) -> VpnClientMultiChannelSession& = delete;
  VpnClientMultiChannelSession(VpnClientMultiChannelSession&&) = delete;
  auto operator=(VpnClientMultiChannelSession&&) -> VpnClientMultiChannelSession& = delete;

  [[nodiscard]] auto GetDescription() const -> std::string { return address; }

  const UdpDynMux::PskType psk;
  const std::string address;
  enum class State : uint8_t {
    kNone,    // StartChannel is not called
    kRunning, // StartChannel is called and channel is established
    kStopped, // StopChannel is called or channel is closed
  } State = State::kNone;

  std::shared_ptr<UdpDynMux::Channel> Channel;
  std::shared_ptr<VpnClientMultiChannel::ChannelSideEndpoint> ChannelSide;
  std::shared_ptr<Pipeline> SessionPipeline;
};

} // namespace gh
