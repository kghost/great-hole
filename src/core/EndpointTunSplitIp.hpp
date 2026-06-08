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

  explicit EndpointTunSplitIp(boost::asio::io_context& ioContext, const std::string& name);
  explicit EndpointTunSplitIp(boost::asio::io_context& ioContext, int testFd);
  ~EndpointTunSplitIp() override;

  EndpointTunSplitIp(const EndpointTunSplitIp&) = delete;
  EndpointTunSplitIp& operator=(const EndpointTunSplitIp&) = delete;
  EndpointTunSplitIp(EndpointTunSplitIp&&) = delete;
  EndpointTunSplitIp& operator=(EndpointTunSplitIp&&) = delete;

  Omni::Fiber::Coroutine<std::shared_ptr<Channel>> CreateChannel(const std::vector<boost::asio::ip::address_v6>& ips);
  Omni::Fiber::Coroutine<void> RemoveChannel(const boost::asio::ip::address_v6& ip);
  Omni::Fiber::Coroutine<void> RemoveChannel(std::shared_ptr<Channel> channel);

protected:
  std::string GetName() const override;
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<void> DoWork() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  Omni::Fiber::Coroutine<void> ReadLoop();
  Omni::Fiber::Coroutine<ErrorCode> WriteToTun(Packet& p, Cancel& c);

  static std::optional<boost::asio::ip::address_v6> GetSourceAddress(const Packet& p);
  static std::optional<boost::asio::ip::address_v6> GetDestAddress(const Packet& p);

  boost::asio::posix::stream_descriptor _TunFileDescriptor;
  const std::string _TunName;
  const int _TestFd{-1};
  std::map<boost::asio::ip::address_v6, std::shared_ptr<Channel>> _Channels;
  Omni::Fiber::RemoteCall _ChannelRpc;
};

class EndpointTunSplitIp::Channel : public Endpoint {
public:
  explicit Channel(EndpointTunSplitIp& parent, const std::vector<boost::asio::ip::address_v6>& ips);
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

  const std::vector<boost::asio::ip::address_v6>& GetIps() const { return _Ips; }

private:
  EndpointTunSplitIp& _Parent;
  const std::vector<boost::asio::ip::address_v6> _Ips;
  Omni::Fiber::Pipe<std::expected<Packet, ErrorCode>> _Pipe;
};

} // namespace gh
