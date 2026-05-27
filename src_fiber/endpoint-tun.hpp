#pragma once

#include <boost/asio.hpp>
#include <boost/asio/posix/basic_descriptor.hpp>

#include "Event.hpp"
#include "endpoint.hpp"

namespace gh {

class Tun : public std::enable_shared_from_this<Tun>, public Endpoint {
public:
  Tun(boost::asio::io_context& io_context, std::string const& name);

  Omni::Fiber::Coroutine<ErrorCode> Start(Omni::Fiber::Event<>& stop_signal) override;
  Omni::Fiber::Coroutine<std::tuple<ErrorCode, Packet>> Read() override;
  Omni::Fiber::Coroutine<std::tuple<ErrorCode, std::size_t>> Write(Packet&&) override;

private:
  Omni::Fiber::Coroutine<ErrorCode> DoStart();

  boost::asio::posix::stream_descriptor _S;

  const std::string _Name;

  bool _IsStarted = false;
  Omni::Fiber::Event<ErrorCode> _StartedError;
};

} // namespace gh
