#pragma once

#include <expected>
#include <map>
#include <memory>

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "Endpoint.hpp"
#include "ErrorCode.hpp"
#include "Pipe.hpp"
#include "RemoteCall.hpp"
#include "Resolver.hpp"
#include "ServiceBase.hpp"

namespace gh {

class Udp : public ServiceBase, public ResolveFor {
public:
  class UdpChannel;

  explicit Udp(boost::asio::any_io_executor executor);
  explicit Udp(boost::asio::any_io_executor executor, boost::asio::ip::udp::endpoint bind);
  ~Udp() override;

  Udp(Udp&) = delete;
  auto operator=(Udp&) -> Udp& = delete;
  Udp(Udp&&) = delete;
  auto operator=(Udp&&) -> Udp& = delete;

  auto GetResolveFor() -> ResolveFor& { return *this; };
  auto GetExecutor() -> boost::asio::any_io_executor override { return _Socket.get_executor(); }
  auto GetService() -> std::string override { return "great_hole_udp"; }
  auto GetProtocol() -> Protocol override { return Protocol::Udp; }

  auto CreateChannel(boost::asio::ip::udp::endpoint const& peer) -> Omni::Fiber::Coroutine<std::shared_ptr<UdpChannel>>;
  auto CreateChannel(std::shared_ptr<ResolverEndpoint> resolver) -> Omni::Fiber::Coroutine<std::shared_ptr<UdpChannel>>;
  auto RemoveChannel(boost::asio::ip::udp::endpoint const& peer) -> Omni::Fiber::Coroutine<void>;
  auto WriteTo(boost::asio::ip::udp::endpoint const& peer, Packet& p, Cancel& c) -> Omni::Fiber::Coroutine<ErrorCode>;
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
  std::map<boost::asio::ip::udp::endpoint, std::shared_ptr<UdpChannel>> _Channels;
  Omni::Fiber::RemoteCall _ChannelRpc;
  std::shared_ptr<Omni::Fiber::Fiber> _ReadLoopFiber;
};

class Udp::UdpChannel : public Endpoint {
public:
  explicit UdpChannel(Udp& parent, boost::asio::ip::udp::endpoint peer);
  ~UdpChannel() override;

  UdpChannel(UdpChannel&) = delete;
  auto operator=(UdpChannel&) -> UdpChannel& = delete;
  UdpChannel(UdpChannel&&) = delete;
  auto operator=(UdpChannel&&) -> UdpChannel& = delete;

  auto Read(Packet& p, Cancel&) -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto Write(Packet& p, Cancel&) -> Omni::Fiber::Coroutine<ErrorCode> override;

  auto GetName() const -> std::string override;
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

public:
  template <typename... Args>
  auto Send(Args&&... args) -> Omni::Fiber::Coroutine<std::expected<void, Omni::Fiber::PipeClosed>> {
    co_return co_await _Pipe.GetProducer().Put(std::forward<Args>(args)...);
  }

private:
  Udp& _Parent;
  boost::asio::ip::udp::endpoint _Peer;
  Omni::Fiber::Pipe<std::expected<Packet, ErrorCode>> _Pipe;
};

} // namespace gh
