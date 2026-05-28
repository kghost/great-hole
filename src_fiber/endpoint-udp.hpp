#pragma once

#include <expected>
#include <functional>
#include <map>
#include <memory>

#include <boost/asio.hpp>

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "Pipe.hpp"
#include "endpoint.hpp"
#include "error-code.hpp"

namespace gh {

class Udp : public std::enable_shared_from_this<Udp> {
public:
  class UdpChannel;

  explicit Udp(boost::asio::io_context& ioContext);
  explicit Udp(boost::asio::io_context& ioContext, boost::asio::ip::udp::endpoint bind);
  ~Udp();

  Udp(Udp&) = delete;
  Udp& operator=(Udp&) = delete;
  Udp(Udp&&) = delete;
  Udp& operator=(Udp&&) = delete;

  Omni::Fiber::Coroutine<ErrorCode> Start();
  Omni::Fiber::Coroutine<ErrorCode> Stop();

  Omni::Fiber::Coroutine<std::shared_ptr<Endpoint>> CreateChannel(boost::asio::ip::udp::endpoint const& peer);
  void RemoveChannel(boost::asio::ip::udp::endpoint const& peer);
  Omni::Fiber::Coroutine<ErrorCode> WriteTo(boost::asio::ip::udp::endpoint const& peer, Packet& p, Cancel& c);
  boost::asio::ip::udp::endpoint LocalEndpoint() const { return _Socket.local_endpoint(); }

private:
  Omni::Fiber::Coroutine<void> ReadLoop();

  boost::asio::ip::udp::socket _Socket;
  boost::asio::ip::udp::endpoint _Local;
  std::map<boost::asio::ip::udp::endpoint, std::weak_ptr<UdpChannel>> _Channels;
  Omni::Fiber::Pipe<std::move_only_function<Omni::Fiber::Coroutine<void>()>> _CreateChannelPipe;
  Cancel _Stop;
};

class Udp::UdpChannel : public Endpoint {
public:
  explicit UdpChannel(std::shared_ptr<Udp> parent, boost::asio::ip::udp::endpoint const& peer);
  ~UdpChannel() override;

  UdpChannel(UdpChannel&) = delete;
  UdpChannel& operator=(UdpChannel&) = delete;
  UdpChannel(UdpChannel&&) = delete;
  UdpChannel& operator=(UdpChannel&&) = delete;

  Omni::Fiber::Coroutine<ErrorCode> Start() override;
  Omni::Fiber::Coroutine<ErrorCode> Stop() override;

  Omni::Fiber::Coroutine<ErrorCode> Read(Packet& p, Cancel&) override;
  Omni::Fiber::Coroutine<ErrorCode> Write(Packet& p, Cancel&) override;

  template <typename... Args> Omni::Fiber::Coroutine<void> Send(Args&&... args) {
    co_return co_await _Pipe.GetProducer().Put(std::forward<Args>(args)...);
  }

private:
  std::shared_ptr<Udp> _Parent;
  boost::asio::ip::udp::endpoint _Peer;
  Omni::Fiber::Pipe<std::expected<Packet, ErrorCode>> _Pipe;
};

} // namespace gh
