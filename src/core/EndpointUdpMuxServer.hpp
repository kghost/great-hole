#pragma once

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
#include "Resolver.hpp"
#include "ServiceBase.hpp"

namespace gh {

class UdpMuxServer : public ServiceBase, public ResolveFor {
public:
  class Channel;

  explicit UdpMuxServer(boost::asio::io_context& ioContext);
  explicit UdpMuxServer(boost::asio::io_context& ioContext, boost::asio::ip::udp::endpoint bind);
  ~UdpMuxServer() override;

  UdpMuxServer(const UdpMuxServer&) = delete;
  UdpMuxServer& operator=(const UdpMuxServer&) = delete;
  UdpMuxServer(UdpMuxServer&&) = delete;
  UdpMuxServer& operator=(UdpMuxServer&&) = delete;

  ResolveFor& GetResolveFor() { return *this; };
  boost::asio::any_io_executor GetExecutor() override { return _Socket.get_executor(); }
  std::string GetService() override { return "great_hole_udp_mux"; }
  Protocol GetProtocol() override { return Protocol::Udp; }

  Omni::Fiber::Coroutine<std::shared_ptr<Endpoint>> CreateChannel(uint8_t id);
  Omni::Fiber::Coroutine<std::shared_ptr<Endpoint>> CreateChannel(uint8_t id, std::shared_ptr<ResolverEndpoint> peer);
  void RemoveChannel(uint8_t id);
  Omni::Fiber::Coroutine<ErrorCode> WriteTo(uint8_t id, Packet& p, Cancel& c);
  boost::asio::ip::udp::endpoint LocalEndpoint() const { return _Socket.local_endpoint(); }

protected:
  std::string GetName() const override;
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<void> DoWork() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  Omni::Fiber::Coroutine<void> ReadLoop();

  struct ChannelInfo {
    std::weak_ptr<Channel> WeakChannel;
    std::optional<boost::asio::ip::udp::endpoint> Peer;
  };

  boost::asio::ip::udp::socket _Socket;
  boost::asio::ip::udp::endpoint _Local;
  std::map<uint8_t, ChannelInfo> _Channels;
  Omni::Fiber::Pipe<std::move_only_function<Omni::Fiber::Coroutine<void>()>> _CreateChannelPipe;
};

class UdpMuxServer::Channel : public Endpoint {
public:
  explicit Channel(std::shared_ptr<UdpMuxServer> parent, uint8_t id);
  explicit Channel(std::shared_ptr<UdpMuxServer> parent, uint8_t id, std::shared_ptr<ResolverEndpoint> peer);
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
  std::shared_ptr<ResolverEndpoint> _PeerResolver = nullptr;
};

} // namespace gh
