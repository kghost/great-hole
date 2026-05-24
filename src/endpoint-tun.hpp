#pragma once

#include <boost/asio.hpp>
#include <boost/asio/posix/basic_descriptor.hpp>

#include "endpoint.hpp"

namespace gh {

class Exec;

class Tun : public std::enable_shared_from_this<Tun>, public Endpoint {
public:
  Tun(boost::asio::io_context& io_context, std::string const& name);
  Tun(boost::asio::io_context& io_context, std::string const& name, std::shared_ptr<Exec> e);

  void AsyncStart(std::move_only_function<Event>&&) override;
  void AsyncRead(std::move_only_function<ReadHandler>&&) override;
  void AsyncWrite(Packet&&, std::move_only_function<WriteHandler>&&) override;

private:
  boost::asio::posix::stream_descriptor _S;

  const std::string _Name;
  std::shared_ptr<Exec> _E;

  bool _Started = false;
  ErrorCode _StartedEc;
};

} // namespace gh
