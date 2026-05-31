#pragma once

#include <chrono>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <random>

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "Endpoint.hpp"
#include "EndpointUdpDynMuxProtocol.hpp"
#include "ErrorCode.hpp"
#include "Pipe.hpp"
#include "RemoteCall.hpp"
#include "Resolver.hpp"
#include "ServiceBase.hpp"

namespace gh {

class UdpDynMux : public ServiceBase, public ResolveFor {
public:
  using PskType = UdpDynMuxProto::PskType;

  class Channel;

  explicit UdpDynMux(boost::asio::io_context& ioContext);
  explicit UdpDynMux(boost::asio::io_context& ioContext, boost::asio::ip::udp::endpoint bind);
  ~UdpDynMux() override;

  UdpDynMux(const UdpDynMux&) = delete;
  UdpDynMux& operator=(const UdpDynMux&) = delete;
  UdpDynMux(UdpDynMux&&) = delete;
  UdpDynMux& operator=(UdpDynMux&&) = delete;

  ResolveFor& GetResolveFor() { return *this; }
  boost::asio::any_io_executor GetExecutor() override { return _Socket.get_executor(); }
  std::string GetService() override { return "great_hole_udp_dyn_mux"; }
  Protocol GetProtocol() override { return Protocol::Udp; }

  Omni::Fiber::Coroutine<std::shared_ptr<Channel>> CreateChannel(const UdpDynMux::PskType& psk);
  Omni::Fiber::Coroutine<std::shared_ptr<Channel>> CreateChannel(const UdpDynMux::PskType& psk,
                                                                 std::shared_ptr<ResolverEndpoint> resolver);
  Omni::Fiber::Coroutine<void> RemoveChannel(const UdpDynMux::PskType& psk);
  Omni::Fiber::Coroutine<ErrorCode> WriteTo(Channel& ch, Packet& p, Cancel& c);
  boost::asio::ip::udp::endpoint LocalEndpoint() const { return _Socket.local_endpoint(); }

protected:
  std::string GetName() const override;
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<void> DoWork() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  Omni::Fiber::Coroutine<void> ReadLoop();

  bool CheckRateLimit(const boost::asio::ip::udp::endpoint& peer);
  uint16_t AllocateUniqueRxId();

  Omni::Fiber::Coroutine<void> SendControlInitiate(const boost::asio::ip::udp::endpoint& peer,
                                                   const UdpDynMux::PskType& psk, uint16_t rxId,
                                                   uint16_t peerRxId);
  Omni::Fiber::Coroutine<void> SendControlKeepalive(const boost::asio::ip::udp::endpoint& peer,
                                                    const UdpDynMux::PskType& psk);
  Omni::Fiber::Coroutine<void> SendControlKeepaliveAck(const boost::asio::ip::udp::endpoint& peer,
                                                       const UdpDynMux::PskType& psk);
  Omni::Fiber::Coroutine<void> SendControlInvalidPsk(const boost::asio::ip::udp::endpoint& peer,
                                                     const UdpDynMux::PskType& psk);
  Omni::Fiber::Coroutine<void> SendControlInvalidChannel(const boost::asio::ip::udp::endpoint& peer,
                                                         uint16_t channelId);

  boost::asio::ip::udp::socket _Socket;
  boost::asio::ip::udp::endpoint _Local;
  std::map<UdpDynMux::PskType, std::shared_ptr<Channel>> _Channels;
  std::map<uint16_t, std::shared_ptr<Channel>> _RxIdToChannel;
  Omni::Fiber::RemoteCall _ChannelRpc;

  // Rate limiting timestamps for control errors based on peer address
  std::map<boost::asio::ip::udp::endpoint, std::chrono::steady_clock::time_point> _LastErrorSent;

  std::mt19937 _Prng;
};

class UdpDynMux::Channel : public Endpoint {
  friend class UdpDynMux;

public:
  enum State { kNegotiating, kRunning };

  explicit Channel(UdpDynMux& parent, const UdpDynMux::PskType& psk, uint16_t rxId,
                   std::shared_ptr<ResolverEndpoint> resolver = nullptr);
  ~Channel() override;

  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;
  Channel(Channel&&) = delete;
  Channel& operator=(Channel&&) = delete;

  Omni::Fiber::Coroutine<ErrorCode> Read(Packet& p, Cancel& c) override;
  Omni::Fiber::Coroutine<ErrorCode> Write(Packet& p, Cancel& c) override;

protected:
  std::string GetName() const override;
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<void> DoWork() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

public:
  template <typename... Args> Omni::Fiber::Coroutine<void> Send(Args&&... args) {
    co_return co_await _Pipe.GetProducer().Put(std::forward<Args>(args)...);
  }

private:
  UdpDynMux& _Parent;
  const UdpDynMux::PskType _Psk;
  const uint16_t _LocalRxId = 0;
  uint16_t _RemoteRxId = 0;
  std::shared_ptr<ResolverEndpoint> _PeerResolver;

  State _State = kNegotiating;
  std::optional<boost::asio::ip::udp::endpoint> _Peer;
  std::chrono::steady_clock::time_point _LastSeen;
  int _MissingAcks = 0;
  std::chrono::steady_clock::time_point _NextKeepaliveTime;
  Omni::Fiber::Pipe<std::expected<Packet, ErrorCode>> _Pipe;
};

} // namespace gh
