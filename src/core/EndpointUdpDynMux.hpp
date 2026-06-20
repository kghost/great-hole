#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <random>

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "Endpoint.hpp"
#include "EndpointUdpDynMuxProtocol.hpp"
#include "ErrorCode.hpp"
#include "Packet.hpp"
#include "Pipe.hpp"
#include "RemoteCall.hpp"
#include "Resolver.hpp"
#include "ServiceBase.hpp"

namespace gh {

class UdpDynMux : public ServiceBase, public ResolveFor {
public:
  using PskType = UdpDynMuxProto::PskType;

  class Channel;

  class ChannelNotification {
  public:
    virtual ~ChannelNotification() = default;
    virtual Omni::Fiber::Coroutine<void> OnChannelEstablished(std::shared_ptr<UdpDynMux::Channel> channel) = 0;
    virtual Omni::Fiber::Coroutine<void> OnChannelClosed(std::shared_ptr<UdpDynMux::Channel> channel) = 0;
  };

  class NoopChannelNotification : public ChannelNotification {
  public:
    ~NoopChannelNotification() override = default;
    Omni::Fiber::Coroutine<void> OnChannelEstablished(std::shared_ptr<UdpDynMux::Channel> channel) override {
      co_return;
    }
    Omni::Fiber::Coroutine<void> OnChannelClosed(std::shared_ptr<UdpDynMux::Channel> channel) override { co_return; }
  };
  static NoopChannelNotification _NoopChannelNotification;

  explicit UdpDynMux(boost::asio::any_io_executor executor,
                     ChannelNotification& notification = _NoopChannelNotification);
  explicit UdpDynMux(boost::asio::any_io_executor executor, boost::asio::ip::udp::endpoint bind,
                     ChannelNotification& notification = _NoopChannelNotification);
  ~UdpDynMux() override;

  UdpDynMux(const UdpDynMux&) = delete;
  UdpDynMux& operator=(const UdpDynMux&) = delete;
  UdpDynMux(UdpDynMux&&) = delete;
  UdpDynMux& operator=(UdpDynMux&&) = delete;

  ResolveFor& GetResolveFor() { return *this; }
  boost::asio::any_io_executor GetExecutor() override { return _Socket.get_executor(); }
  std::string GetService() override { return "great_hole_udp_dyn_mux"; }
  Protocol GetProtocol() override { return Protocol::Udp; }

  void SetChannelNotification(ChannelNotification& notification) { _Notification = notification; }

  Omni::Fiber::Coroutine<std::shared_ptr<Channel>> CreateChannel(const UdpDynMux::PskType& psk);
  Omni::Fiber::Coroutine<std::shared_ptr<Channel>> CreateChannel(const UdpDynMux::PskType& psk,
                                                                 std::shared_ptr<ResolverEndpoint> resolver);
  Omni::Fiber::Coroutine<void> RemoveChannel(const UdpDynMux::PskType& psk);
  Omni::Fiber::Coroutine<ErrorCode> WriteTo(boost::asio::ip::udp::endpoint peer, Packet& p, Cancel& c);
  boost::asio::ip::udp::endpoint LocalEndpoint() const { return _Socket.local_endpoint(); }

  std::string GetName() const override;

protected:
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<void> DoWork() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  Omni::Fiber::Coroutine<void> ReadLoop();

  bool CheckRateLimit(const boost::asio::ip::udp::endpoint& peer);
  uint16_t AllocateUniqueRxId();

  Omni::Fiber::Coroutine<void> SendControlInitiate(const boost::asio::ip::udp::endpoint& peer,
                                                   const UdpDynMux::PskType& psk, uint16_t rxId, uint16_t peerRxId);
  Omni::Fiber::Coroutine<void> SendControlInitiateFail(const boost::asio::ip::udp::endpoint& peer,
                                                       const UdpDynMux::PskType& psk);
  Omni::Fiber::Coroutine<void> SendControlKeepalive(const boost::asio::ip::udp::endpoint& peer,
                                                    const UdpDynMux::PskType& psk, uint8_t flags);
  Omni::Fiber::Coroutine<void> SendControlInvalidPsk(const boost::asio::ip::udp::endpoint& peer,
                                                     const UdpDynMux::PskType& psk);
  Omni::Fiber::Coroutine<void> SendControlInvalidChannel(const boost::asio::ip::udp::endpoint& peer,
                                                         uint16_t channelId);
  Omni::Fiber::Coroutine<void> SendControlInvalidAddress(const boost::asio::ip::udp::endpoint& peer,
                                                         uint16_t channelId);

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
  enum class State { kNegotiating, kRunning, kStopping };

  explicit Channel(UdpDynMux& parent, const UdpDynMux::PskType& psk, uint16_t rxId,
                   std::shared_ptr<ResolverEndpoint> resolver = nullptr);
  ~Channel() override;

  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;
  Channel(Channel&&) = delete;
  Channel& operator=(Channel&&) = delete;

  std::string GetName() const override;
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<void> DoWork() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

  UdpDynMux::PskType GetPsk() const { return _Psk; }
  uint16_t GetLocalRxId() const { return _LocalRxId; }
  uint16_t GetRemoteRxId() const { return _RemoteRxId; }

  Omni::Fiber::Coroutine<ErrorCode> Read(Packet& p, Cancel& c) override;
  Omni::Fiber::Coroutine<ErrorCode> Write(Packet& p, Cancel& c) override;

private:
  UdpDynMux& _Parent;
  const UdpDynMux::PskType _Psk;
  const uint16_t _LocalRxId = 0;
  uint16_t _RemoteRxId = 0;
  std::shared_ptr<ResolverEndpoint> _PeerResolver;

  State _State = State::kNegotiating;
  std::optional<boost::asio::ip::udp::endpoint> _Peer;
  std::chrono::steady_clock::time_point _LastSeen;
  std::chrono::steady_clock::time_point _NextKeepaliveTime;
  std::optional<std::chrono::steady_clock::time_point> _LastPingSentTime;
  Omni::Fiber::Pipe<Packet> _DataPacket;
  Omni::Fiber::Pipe<std::tuple<boost::asio::ip::udp::endpoint, Packet>> _ControlPacket;

  Omni::Fiber::Coroutine<State> DoWorkNegotiating();
  Omni::Fiber::Coroutine<State> DoWorkRunning();
  Omni::Fiber::Coroutine<UdpDynMux::Channel::State> HandleControlPacket(boost::asio::ip::udp::endpoint, Packet&);
};

} // namespace gh
