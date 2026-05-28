#pragma once

#include <boost/asio.hpp>
#include <boost/asio/posix/basic_descriptor.hpp>

#include "endpoint.hpp"

namespace gh {

class Tun : public std::enable_shared_from_this<Tun>, public Endpoint {
public:
  Tun(boost::asio::io_context& io_context, std::string const& name);

  Omni::Fiber::Coroutine<ErrorCode> Start() override;
  Omni::Fiber::Coroutine<ErrorCode> Stop() override;
  Omni::Fiber::Coroutine<ErrorCode> Read(Packet& p, Cancel&) override;
  Omni::Fiber::Coroutine<ErrorCode> Write(Packet& p, Cancel&) override;

private:
  boost::asio::posix::stream_descriptor _TunFileDescriptor;
  const std::string _Name;
};

} // namespace gh
