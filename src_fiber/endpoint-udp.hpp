#pragma once

#include <expected>
#include <map>
#include <memory>

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "Event.hpp"
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

  std::shared_ptr<Endpoint> CreateChannel(boost::asio::ip::udp::endpoint const& peer);

  Omni::Fiber::Coroutine<ErrorCode> StartChannel(std::shared_ptr<UdpChannel> channel);
  void StopChannel(boost::asio::ip::udp::endpoint const& peer);
  void RemoveChannel(boost::asio::ip::udp::endpoint const& peer);
  Omni::Fiber::Coroutine<ErrorCode> WriteTo(boost::asio::ip::udp::endpoint const& peer, Packet& p);

private:
  Omni::Fiber::Coroutine<ErrorCode> DoStart();
  void DoStop();
  Omni::Fiber::Coroutine<void> ReadLoop();

  boost::asio::ip::udp::socket _Socket;
  boost::asio::ip::udp::endpoint _Local;

  std::map<boost::asio::ip::udp::endpoint, std::weak_ptr<UdpChannel>> _Channels;
  std::map<boost::asio::ip::udp::endpoint, std::weak_ptr<UdpChannel>> _ActiveChannels;

  bool _IsStarted = false;
  Omni::Fiber::Event<ErrorCode> _StartedError;
  Omni::Fiber::Event<> _StopSignal;
};

class Udp::UdpChannel : public std::enable_shared_from_this<UdpChannel>, public Endpoint {
public:
  explicit UdpChannel(std::shared_ptr<Udp> parent, boost::asio::ip::udp::endpoint const& peer);
  ~UdpChannel() override;

  UdpChannel(UdpChannel&) = delete;
  UdpChannel& operator=(UdpChannel&) = delete;
  UdpChannel(UdpChannel&&) = delete;
  UdpChannel& operator=(UdpChannel&&) = delete;

  Omni::Fiber::Coroutine<ErrorCode> Start(Omni::Fiber::Event<>& stopSignal) override;
  Omni::Fiber::Coroutine<ErrorCode> Read(Packet& p) override;
  Omni::Fiber::Coroutine<ErrorCode> Write(Packet& p) override;

  boost::asio::ip::udp::endpoint const& GetPeer() const { return _Peer; }

  template <typename... Args> Omni::Fiber::Coroutine<void> Send(Args&&... args) {
    co_return co_await _Pipe.GetProducer().Put(std::forward<Args>(args)...);
  }

private:
  std::shared_ptr<Udp> _Parent;
  boost::asio::ip::udp::endpoint _Peer;

  Omni::Fiber::Pipe<std::expected<Packet, ErrorCode>> _Pipe;

  bool _IsStarted = false;
  Omni::Fiber::Event<ErrorCode> _StartedError;
};

} // namespace gh
