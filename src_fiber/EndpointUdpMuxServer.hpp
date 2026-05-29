#pragma once

#include <expected>
#include <functional>
#include <map>
#include <memory>

#include <boost/asio.hpp>

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "Endpoint.hpp"
#include "ErrorCode.hpp"
#include "Pipe.hpp"

namespace gh {

class UdpMuxServer : public std::enable_shared_from_this<UdpMuxServer> {
public:
  class Channel;

  explicit UdpMuxServer(boost::asio::io_context& ioContext);
  explicit UdpMuxServer(boost::asio::io_context& ioContext, boost::asio::ip::udp::endpoint bind);
  ~UdpMuxServer();

  UdpMuxServer(const UdpMuxServer&) = delete;
  UdpMuxServer& operator=(const UdpMuxServer&) = delete;
  UdpMuxServer(UdpMuxServer&&) = delete;
  UdpMuxServer& operator=(UdpMuxServer&&) = delete;

  Omni::Fiber::Coroutine<ErrorCode> Start();
  Omni::Fiber::Coroutine<ErrorCode> Stop();

  Omni::Fiber::Coroutine<std::shared_ptr<Endpoint>> CreateChannel(uint8_t id);
  void RemoveChannel(uint8_t id);
  Omni::Fiber::Coroutine<ErrorCode> WriteTo(uint8_t id, Packet& p, Cancel& c);
  boost::asio::ip::udp::endpoint LocalEndpoint() const { return _Socket.local_endpoint(); }

private:
  Omni::Fiber::Coroutine<void> ReadLoop();

  boost::asio::ip::udp::socket _Socket;
  boost::asio::ip::udp::endpoint _Local;
  std::map<uint8_t, std::weak_ptr<Channel>> _Channels;
  std::map<uint8_t, boost::asio::ip::udp::endpoint> _Peers;
  Omni::Fiber::Pipe<std::move_only_function<Omni::Fiber::Coroutine<void>()>> _CreateChannelPipe;
  Cancel _Stop;
};

class UdpMuxServer::Channel : public Endpoint {
public:
  explicit Channel(std::shared_ptr<UdpMuxServer> parent, uint8_t id);
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
  std::shared_ptr<UdpMuxServer> _Parent;
  uint8_t _Id;
  Omni::Fiber::Pipe<std::expected<Packet, ErrorCode>> _Pipe;
};

} // namespace gh
