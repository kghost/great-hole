#pragma once

#include <expected>
#include <map>
#include <memory>
#include <optional>

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "Endpoint.hpp"
#include "ErrorCode.hpp"
#include "Pipe.hpp"
#include "RemoteCall.hpp"
#include "Resolver.hpp"

namespace gh {

class UdpMux : public ServiceBase, public ResolveFor {
public:
  class Channel;

  explicit UdpMux(boost::asio::io_context& ioContext);
  explicit UdpMux(boost::asio::io_context& ioContext, boost::asio::ip::udp::endpoint bind);
  ~UdpMux() override;

  UdpMux(const UdpMux&) = delete;
  UdpMux& operator=(const UdpMux&) = delete;
  UdpMux(UdpMux&&) = delete;
  UdpMux& operator=(UdpMux&&) = delete;

  ResolveFor& GetResolveFor() { return *this; };
  boost::asio::any_io_executor GetExecutor() override { return _Socket.get_executor(); }
  std::string GetService() override { return "great_hole_udp_mux"; }
  Protocol GetProtocol() override { return Protocol::Udp; }

  Omni::Fiber::Coroutine<std::shared_ptr<Channel>> CreateChannel(uint8_t id);
  Omni::Fiber::Coroutine<std::shared_ptr<Channel>> CreateChannel(uint8_t id, std::shared_ptr<ResolverEndpoint> peer);
  Omni::Fiber::Coroutine<void> RemoveChannel(uint8_t id);
  Omni::Fiber::Coroutine<ErrorCode> WriteTo(uint8_t id, Packet& p, Cancel& c);
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
  std::map<uint8_t, std::shared_ptr<Channel>> _Channels;
  Omni::Fiber::RemoteCall _ChannelRpc;
};

class UdpMux::Channel : public Endpoint {
public:
  explicit Channel(UdpMux& parent, uint8_t id);
  explicit Channel(UdpMux& parent, uint8_t id, boost::asio::ip::udp::endpoint peer);
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
    auto reply = co_await _Pipe.GetProducer().Put(std::forward<Args>(args)...);
    assert(reply.has_value());
    co_return;
  }

  std::optional<boost::asio::ip::udp::endpoint>& GetPeer() { return _Peer; }

private:
  UdpMux& _Parent;
  uint8_t _Id;
  Omni::Fiber::Pipe<std::expected<Packet, ErrorCode>> _Pipe;
  std::optional<boost::asio::ip::udp::endpoint> _Peer;
};

} // namespace gh
