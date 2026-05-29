#pragma once

#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <variant>

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "Endpoint.hpp"
#include "ErrorCode.hpp"
#include "Pipe.hpp"
#include "ServiceBase.hpp"
#include "resolvers/Resolver.hpp"

namespace gh {

class Udp : public ServiceBase, public ResolveFor {
public:
  class UdpChannel;

  explicit Udp(boost::asio::io_context& ioContext);
  explicit Udp(boost::asio::io_context& ioContext, boost::asio::ip::udp::endpoint bind);
  ~Udp() override;

  Udp(Udp&) = delete;
  Udp& operator=(Udp&) = delete;
  Udp(Udp&&) = delete;
  Udp& operator=(Udp&&) = delete;

  ResolveFor& GetResolveFor() { return *this; };
  boost::asio::any_io_executor GetExecutor() override { return _Socket.get_executor(); }
  std::string GetService() override { return "great_hole_udp"; }
  Protocol GetProtocol() override { return Protocol::Udp; }

  Omni::Fiber::Coroutine<std::shared_ptr<Endpoint>> CreateChannel(boost::asio::ip::udp::endpoint const& peer);
  Omni::Fiber::Coroutine<std::shared_ptr<Endpoint>> CreateChannel(std::shared_ptr<ResolverEndpoint> resolver);
  void RemoveChannel(boost::asio::ip::udp::endpoint const& peer);
  void AddChannel(boost::asio::ip::udp::endpoint const& peer, std::shared_ptr<UdpChannel> ch);
  Omni::Fiber::Coroutine<ErrorCode> WriteTo(boost::asio::ip::udp::endpoint const& peer, Packet& p, Cancel& c);
  boost::asio::ip::udp::endpoint LocalEndpoint() const { return _Socket.local_endpoint(); }

protected:
  std::string GetName() const override;
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<void> DoWork() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  Omni::Fiber::Coroutine<void> ReadLoop();

  boost::asio::ip::udp::socket _Socket;
  boost::asio::ip::udp::endpoint _Local;
  std::map<boost::asio::ip::udp::endpoint, std::weak_ptr<UdpChannel>> _Channels;
  Omni::Fiber::Pipe<std::move_only_function<Omni::Fiber::Coroutine<void>()>> _CreateChannelPipe;
};

class Udp::UdpChannel : public Endpoint {
public:
  using Target = std::variant<std::shared_ptr<ResolverEndpoint>, boost::asio::ip::udp::endpoint>;

  explicit UdpChannel(std::shared_ptr<Udp> parent, Target target);
  ~UdpChannel() override;

  UdpChannel(UdpChannel&) = delete;
  UdpChannel& operator=(UdpChannel&) = delete;
  UdpChannel(UdpChannel&&) = delete;
  UdpChannel& operator=(UdpChannel&&) = delete;

  Omni::Fiber::Coroutine<ErrorCode> Read(Packet& p, Cancel&) override;
  Omni::Fiber::Coroutine<ErrorCode> Write(Packet& p, Cancel&) override;

protected:
  std::string GetName() const override;
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

public:
  template <typename... Args> Omni::Fiber::Coroutine<void> Send(Args&&... args) {
    co_return co_await _Pipe.GetProducer().Put(std::forward<Args>(args)...);
  }

private:
  std::shared_ptr<Udp> _Parent;
  Target _Peer;
  Omni::Fiber::Pipe<std::expected<Packet, ErrorCode>> _Pipe;
};

} // namespace gh
