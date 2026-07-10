#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/posix/basic_descriptor.hpp>

#include "Coroutine.hpp"
#include "Endpoint.hpp"
#include "ErrorCode.hpp"
#include "Pipe.hpp"
#include "RemoteCall.hpp"
#include "ServiceBase.hpp"

namespace gh {

class EndpointTunSplitIp : public ServiceBase {
public:
  class Channel;

  explicit EndpointTunSplitIp(boost::asio::any_io_executor executor, const std::string& name);
  explicit EndpointTunSplitIp(boost::asio::any_io_executor executor, const std::string& name, int fd);
  ~EndpointTunSplitIp() override;

  EndpointTunSplitIp(const EndpointTunSplitIp&) = delete;
  auto operator=(const EndpointTunSplitIp&) -> EndpointTunSplitIp& = delete;
  EndpointTunSplitIp(EndpointTunSplitIp&&) = delete;
  auto operator=(EndpointTunSplitIp&&) -> EndpointTunSplitIp& = delete;

  auto CreateChannel(const std::vector<boost::asio::ip::address_v6>& ips) -> Omni::Fiber::Coroutine<std::shared_ptr<Channel>>;
  auto RemoveChannel(std::shared_ptr<Channel> channel) -> Omni::Fiber::Coroutine<void>;

  auto GetName() const -> std::string override;

protected:
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  auto ReadLoop() -> Omni::Fiber::Coroutine<void>;
  auto WriteToTun(Packet& p, Cancel& c) -> Omni::Fiber::Coroutine<ErrorCode>;

  static auto GetSourceAddress(const Packet& p) -> std::optional<boost::asio::ip::address_v6>;
  static auto GetDestAddress(const Packet& p) -> std::optional<boost::asio::ip::address_v6>;

  boost::asio::posix::stream_descriptor _TunFileDescriptor;
  const std::string _TunName;
  std::map<boost::asio::ip::address_v6, std::shared_ptr<Channel>> _Channels;
  Omni::Fiber::RemoteCall _ChannelRpc;
  std::shared_ptr<Omni::Fiber::Fiber> _ReadLoopFiber;
};

class EndpointTunSplitIp::Channel : public Endpoint {
public:
  explicit Channel(EndpointTunSplitIp& parent, const std::vector<boost::asio::ip::address_v6>& ips);
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

  auto GetIps() const -> const std::vector<boost::asio::ip::address_v6>& { return _Ips; }

private:
  EndpointTunSplitIp& _Parent;
  const std::vector<boost::asio::ip::address_v6> _Ips;
  Omni::Fiber::Pipe<std::expected<Packet, ErrorCode>> _Pipe;
};

} // namespace gh
