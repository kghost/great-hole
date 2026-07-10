#pragma once

#include <boost/asio.hpp>
#include <boost/asio/posix/basic_descriptor.hpp>

#include "Endpoint.hpp"

namespace gh {

class Tun : public Endpoint {
public:
  Tun(boost::asio::any_io_executor executor, std::string const& name);
  Tun(boost::asio::any_io_executor executor, std::string const& name, int fd);

  auto Read(Packet& p, Cancel&) -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto Write(Packet& p, Cancel&) -> Omni::Fiber::Coroutine<ErrorCode> override;

protected:
  auto GetName() const -> std::string override;
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  boost::asio::posix::stream_descriptor _TunFileDescriptor;
  const std::string _Name;
};

} // namespace gh
