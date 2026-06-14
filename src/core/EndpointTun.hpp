#pragma once

#include <boost/asio.hpp>
#include <boost/asio/posix/basic_descriptor.hpp>

#include "Endpoint.hpp"

namespace gh {

class Tun : public Endpoint {
public:
  Tun(boost::asio::any_io_executor executor, std::string const& name);
  Tun(boost::asio::any_io_executor executor, int fd);

  Omni::Fiber::Coroutine<ErrorCode> Read(Packet& p, Cancel&) override;
  Omni::Fiber::Coroutine<ErrorCode> Write(Packet& p, Cancel&) override;

protected:
  std::string GetName() const override;
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  boost::asio::posix::stream_descriptor _TunFileDescriptor;
  const std::string _Name;
};

} // namespace gh
