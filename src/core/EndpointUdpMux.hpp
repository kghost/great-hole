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

  explicit UdpMux(boost::asio::any_io_executor executor);
  explicit UdpMux(boost::asio::any_io_executor executor, boost::asio::ip::udp::endpoint bind);
  ~UdpMux() override;

  UdpMux(const UdpMux&) = delete;
  auto operator=(const UdpMux&) -> UdpMux& = delete;
  UdpMux(UdpMux&&) = delete;
  auto operator=(UdpMux&&) -> UdpMux& = delete;

  auto GetResolveFor() -> ResolveFor& { return *this; };
  auto GetExecutor() -> boost::asio::any_io_executor override { return _Socket.get_executor(); }
  auto GetService() -> std::string override { return "great_hole_udp_mux"; }
  auto GetProtocol() -> Protocol override { return Protocol::Udp; }

  auto CreateChannel(uint8_t id) -> Omni::Fiber::Coroutine<std::shared_ptr<Channel>>;
  auto CreateChannel(uint8_t id, std::shared_ptr<ResolverEndpoint> peer) -> Omni::Fiber::Coroutine<std::shared_ptr<Channel>>;
  auto RemoveChannel(uint8_t id) -> Omni::Fiber::Coroutine<void>;
  auto WriteTo(uint8_t id, Packet& p, Cancel& c) -> Omni::Fiber::Coroutine<ErrorCode>;
  auto LocalEndpoint() const -> boost::asio::ip::udp::endpoint { return _Socket.local_endpoint(); }

protected:
  auto GetName() const -> std::string override;
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  auto ReadLoop() -> Omni::Fiber::Coroutine<void>;

  boost::asio::ip::udp::socket _Socket;
  boost::asio::ip::udp::endpoint _Local;
  std::map<uint8_t, std::shared_ptr<Channel>> _Channels;
  Omni::Fiber::RemoteCall _ChannelRpc;
  std::shared_ptr<Omni::Fiber::Fiber> _ReadLoopFiber;
};

class UdpMux::Channel : public Endpoint {
public:
  explicit Channel(UdpMux& parent, uint8_t id);
  explicit Channel(UdpMux& parent, uint8_t id, boost::asio::ip::udp::endpoint peer);
  ~Channel() override;

  Channel(const Channel&) = delete;
  auto operator=(const Channel&) -> Channel& = delete;
  Channel(Channel&&) = delete;
  auto operator=(Channel&&) -> Channel& = delete;

  auto Read(Packet& p, Cancel& c) -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto Write(Packet& p, Cancel& c) -> Omni::Fiber::Coroutine<ErrorCode> override;

  auto GetName() const -> std::string override;
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

public:
  template <typename... Args>
  auto Send(Args&&... args) -> Omni::Fiber::Coroutine<std::expected<void, Omni::Fiber::PipeClosed>> {
    co_return co_await _Pipe.GetProducer().Put(std::forward<Args>(args)...);
  }

  auto GetPeer() -> std::optional<boost::asio::ip::udp::endpoint>& { return _Peer; }

private:
  UdpMux& _Parent;
  uint8_t _Id;
  Omni::Fiber::Pipe<std::expected<Packet, ErrorCode>> _Pipe;
  std::optional<boost::asio::ip::udp::endpoint> _Peer;
};

} // namespace gh
