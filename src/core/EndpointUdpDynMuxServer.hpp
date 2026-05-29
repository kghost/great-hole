#pragma once

#include <chrono>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "Endpoint.hpp"
#include "ErrorCode.hpp"
#include "Pipe.hpp"
#include "ServiceBase.hpp"

namespace gh {

class UdpDynMuxServer : public ServiceBase {
public:
  class Channel;

  explicit UdpDynMuxServer(boost::asio::io_context& ioContext);
  explicit UdpDynMuxServer(boost::asio::io_context& ioContext, boost::asio::ip::udp::endpoint bind);
  ~UdpDynMuxServer() override;

  UdpDynMuxServer(const UdpDynMuxServer&) = delete;
  UdpDynMuxServer& operator=(const UdpDynMuxServer&) = delete;
  UdpDynMuxServer(UdpDynMuxServer&&) = delete;
  UdpDynMuxServer& operator=(UdpDynMuxServer&&) = delete;

  Omni::Fiber::Coroutine<std::shared_ptr<Endpoint>> CreateChannel(uint16_t id);
  void RemoveChannel(uint16_t id);
  Omni::Fiber::Coroutine<ErrorCode> WriteTo(uint16_t id, Packet& p, Cancel& c);
  boost::asio::ip::udp::endpoint LocalEndpoint() const { return _Socket.local_endpoint(); }

protected:
  std::string GetName() const override;
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<void> DoWork() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  Omni::Fiber::Coroutine<void> ReadLoop();
  Omni::Fiber::Coroutine<void> KeepaliveLoop();

  struct ChannelInfo {
    std::weak_ptr<Channel> WeakChannel;
    std::optional<boost::asio::ip::udp::endpoint> Peer;
    uint32_t Cookie = 0;
    std::chrono::steady_clock::time_point LastSeen;
    int MissingAcks = 0;
  };

  boost::asio::ip::udp::socket _Socket;
  boost::asio::ip::udp::endpoint _Local;
  std::map<uint16_t, ChannelInfo> _Channels;
  Omni::Fiber::Pipe<std::move_only_function<Omni::Fiber::Coroutine<void>()>> _CreateChannelPipe;

  // Rate limiting timestamps for control errors based on peer address
  std::map<boost::asio::ip::udp::endpoint, std::chrono::steady_clock::time_point> _LastErrorSent;

  bool CheckRateLimit(const boost::asio::ip::udp::endpoint& peer);

  Omni::Fiber::Coroutine<void> SendControlAssign(const boost::asio::ip::udp::endpoint& peer, uint32_t cookie, uint16_t id);
  Omni::Fiber::Coroutine<void> SendControlKeepalive(const boost::asio::ip::udp::endpoint& peer, uint16_t id);
  Omni::Fiber::Coroutine<void> SendControlIdClosed(const boost::asio::ip::udp::endpoint& peer, uint16_t id);
  Omni::Fiber::Coroutine<void> SendControlAddrMismatch(const boost::asio::ip::udp::endpoint& peer, uint16_t id);
  Omni::Fiber::Coroutine<void> SendControlMigrateAck(const boost::asio::ip::udp::endpoint& peer, uint16_t id);
  Omni::Fiber::Coroutine<void> SendControlInvalidId(const boost::asio::ip::udp::endpoint& peer, uint16_t id);
  Omni::Fiber::Coroutine<void> SendControlCookieMismatch(const boost::asio::ip::udp::endpoint& peer, uint16_t id);
};

class UdpDynMuxServer::Channel : public Endpoint {
public:
  explicit Channel(std::shared_ptr<UdpDynMuxServer> parent, uint16_t id);
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
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

public:
  template <typename... Args> Omni::Fiber::Coroutine<void> Send(Args&&... args) {
    co_return co_await _Pipe.GetProducer().Put(std::forward<Args>(args)...);
  }

private:
  std::shared_ptr<UdpDynMuxServer> _Parent;
  uint16_t _Id;
  Omni::Fiber::Pipe<std::expected<Packet, ErrorCode>> _Pipe;
};

} // namespace gh
