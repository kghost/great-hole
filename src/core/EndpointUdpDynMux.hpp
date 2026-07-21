#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "Endpoint.hpp"
#include "ErrorCode.hpp"
#include "Packet.hpp"
#include "Pipe.hpp"
#include "RemoteCall.hpp"
#include "Resolver.hpp"
#include "ServiceBase.hpp"

namespace gh {

class UdpDynMux : public ServiceBase, public ResolveFor {
public:
  static constexpr size_t kPskSize = 16;
  using PskType = std::array<uint8_t, kPskSize>;

  class Channel;

  class ChannelNotification {
  public:
    explicit ChannelNotification() = default;
    virtual ~ChannelNotification() = default;

    ChannelNotification(const ChannelNotification&) = delete;
    auto operator=(const ChannelNotification&) -> ChannelNotification& = delete;
    ChannelNotification(ChannelNotification&&) = delete;
    auto operator=(ChannelNotification&&) -> ChannelNotification& = delete;

    virtual auto OnChannelEstablished(std::shared_ptr<UdpDynMux::Channel> channel) -> Omni::Fiber::Coroutine<void> = 0;
    virtual auto OnChannelClosed(std::shared_ptr<UdpDynMux::Channel> channel) -> Omni::Fiber::Coroutine<void> = 0;
  };

  class NoopChannelNotification : public ChannelNotification {
  public:
    explicit NoopChannelNotification() = default;
    ~NoopChannelNotification() override = default;

    NoopChannelNotification(const NoopChannelNotification&) = delete;
    auto operator=(const NoopChannelNotification&) -> NoopChannelNotification& = delete;
    NoopChannelNotification(NoopChannelNotification&&) = delete;
    auto operator=(NoopChannelNotification&&) -> NoopChannelNotification& = delete;

    auto OnChannelEstablished(std::shared_ptr<UdpDynMux::Channel> channel) -> Omni::Fiber::Coroutine<void> override {
      co_return;
    }
    auto OnChannelClosed(std::shared_ptr<UdpDynMux::Channel> channel) -> Omni::Fiber::Coroutine<void> override {
      co_return;
    }
  };
  static NoopChannelNotification _NoopChannelNotification;

  explicit UdpDynMux(boost::asio::any_io_executor executor,
                     ChannelNotification& notification = _NoopChannelNotification);
  explicit UdpDynMux(boost::asio::any_io_executor executor, boost::asio::ip::udp::endpoint bind,
                     ChannelNotification& notification = _NoopChannelNotification);
  ~UdpDynMux() override;

  UdpDynMux(const UdpDynMux&) = delete;
  auto operator=(const UdpDynMux&) -> UdpDynMux& = delete;
  UdpDynMux(UdpDynMux&&) = delete;
  auto operator=(UdpDynMux&&) -> UdpDynMux& = delete;

  auto GetResolveFor() -> ResolveFor& { return *this; }
  auto GetExecutor() -> boost::asio::any_io_executor override { return _Socket.get_executor(); }
  auto GetService() -> std::string override { return "great_hole_udp_dyn_mux"; }
  auto GetProtocol() -> Protocol override { return Protocol::Udp; }

  void SetChannelNotification(ChannelNotification& notification) { _Notification = notification; }

  auto CreateChannel(const UdpDynMux::PskType& psk) -> Omni::Fiber::Coroutine<std::shared_ptr<Channel>>;
  auto CreateChannel(const UdpDynMux::PskType& psk, std::shared_ptr<ResolverEndpoint> resolver)
      -> Omni::Fiber::Coroutine<std::shared_ptr<Channel>>;
  // TODO: this function should take a std::shared_ptr<Channel> argument
  auto RemoveChannel(const UdpDynMux::PskType& psk) -> Omni::Fiber::Coroutine<void>;
  auto WriteTo(boost::asio::ip::udp::endpoint peer, Packet& packet, Cancel& cancel)
      -> Omni::Fiber::Coroutine<ErrorCode>;
  auto LocalEndpoint() const -> boost::asio::ip::udp::endpoint { return _Socket.local_endpoint(); }

  auto GetName() const -> std::string override;

protected:
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  auto ReadLoop() -> Omni::Fiber::Coroutine<void>;

  auto CheckRateLimit(const boost::asio::ip::udp::endpoint& peer) -> bool;
  auto AllocateUniqueRxId() -> uint16_t;

  auto SendControlInitiate(const boost::asio::ip::udp::endpoint& peer, const UdpDynMux::PskType& psk, uint16_t rxId,
                           uint16_t peerRxId) -> Omni::Fiber::Coroutine<void>;
  auto SendControlInitiateFail(const boost::asio::ip::udp::endpoint& peer, const UdpDynMux::PskType& psk)
      -> Omni::Fiber::Coroutine<void>;
  auto SendControlKeepalive(const boost::asio::ip::udp::endpoint& peer, const UdpDynMux::PskType& psk, uint8_t flags)
      -> Omni::Fiber::Coroutine<void>;
  auto SendControlInvalidPsk(const boost::asio::ip::udp::endpoint& peer, const UdpDynMux::PskType& psk)
      -> Omni::Fiber::Coroutine<void>;
  auto SendControlInvalidChannel(const boost::asio::ip::udp::endpoint& peer, uint16_t channelId)
      -> Omni::Fiber::Coroutine<void>;
  auto SendControlInvalidAddress(const boost::asio::ip::udp::endpoint& peer, uint16_t channelId)
      -> Omni::Fiber::Coroutine<void>;

  std::reference_wrapper<ChannelNotification> _Notification;
  boost::asio::ip::udp::socket _Socket;
  boost::asio::ip::udp::endpoint _Local;
  std::map<UdpDynMux::PskType, std::shared_ptr<Channel>> _Channels;
  std::map<uint16_t, std::shared_ptr<Channel>> _RxIdToChannel;
  Omni::Fiber::RemoteCall _ChannelRpc;

  // Rate limiting timestamps for control errors based on peer address
  std::map<boost::asio::ip::udp::endpoint, std::chrono::steady_clock::time_point> _LastErrorSent;

  std::mt19937 _Prng;
  std::shared_ptr<Omni::Fiber::Fiber> _ReadLoopFiber;
};

class UdpDynMux::Channel : public Endpoint {
  friend class UdpDynMux;

public:
  enum class State : uint8_t { kNegotiating, kRunning, kStopping };

  explicit Channel(UdpDynMux& parent, const UdpDynMux::PskType& psk, uint16_t rxId,
                   std::shared_ptr<ResolverEndpoint> resolver = nullptr);
  ~Channel() override;

  Channel(const Channel&) = delete;
  auto operator=(const Channel&) -> Channel& = delete;
  Channel(Channel&&) = delete;
  auto operator=(Channel&&) -> Channel& = delete;

  auto GetName() const -> std::string override;
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

  auto GetPsk() const -> UdpDynMux::PskType { return _Psk; }
  auto GetLocalRxId() const -> uint16_t { return _LocalRxId; }
  auto GetRemoteRxId() const -> uint16_t { return _RemoteRxId; }
  auto GetRoundTripTime() const -> std::chrono::milliseconds { return _RoundTripTime; }
  auto GetChannelState() const -> State { return _State; }

  auto Read(Packet& packet, Cancel& cancel) -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto Write(Packet& packet, Cancel& cancel) -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  UdpDynMux& _Parent;
  const UdpDynMux::PskType _Psk;
  const uint16_t _LocalRxId = 0;
  uint16_t _RemoteRxId = 0;
  std::shared_ptr<ResolverEndpoint> _PeerResolver;

  State _State = State::kNegotiating;
  std::optional<boost::asio::ip::udp::endpoint> _Peer;
  std::chrono::steady_clock::time_point _LastSeen;
  std::chrono::steady_clock::time_point _LastPingSentTime;
  std::chrono::steady_clock::time_point _NextKeepaliveSilentTime;
  std::chrono::steady_clock::time_point _NextKeepaliveTime;
  Omni::Fiber::Pipe<Packet> _DataPacket;
  Omni::Fiber::Pipe<std::tuple<boost::asio::ip::udp::endpoint, Packet>> _ControlPacket;
  std::chrono::milliseconds _RoundTripTime{0};

  static constexpr std::chrono::seconds MinKeepaliveInterval = std::chrono::seconds(60);
  static constexpr std::chrono::seconds MaxKeepaliveInterval = std::chrono::seconds(90);
  static constexpr std::chrono::seconds KeepaliveTimeout = 3 * MaxKeepaliveInterval;

  auto DoWorkNegotiating() -> Omni::Fiber::Coroutine<State>;
  auto DoWorkRunning() -> Omni::Fiber::Coroutine<State>;
  auto HandleControlPacket(boost::asio::ip::udp::endpoint, Packet&)
      -> Omni::Fiber::Coroutine<UdpDynMux::Channel::State>;
  void AdjustKeepaliveTimers(std::chrono::steady_clock::time_point now) {
    _LastPingSentTime = now;
    _NextKeepaliveSilentTime = now + MinKeepaliveInterval;
    std::uniform_int_distribution<long long> dist(
        std::chrono::duration_cast<std::chrono::seconds>(MinKeepaliveInterval).count(),
        std::chrono::duration_cast<std::chrono::seconds>(MaxKeepaliveInterval).count());
    _NextKeepaliveTime = now + std::chrono::seconds(dist(_Parent._Prng));
  }
};

} // namespace gh
