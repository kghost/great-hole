#pragma once

#include "endpoint.hpp"
#include "util-exec.hpp"

namespace gh {

class EndpointExec : public Endpoint {
public:
  void AsyncStart(std::move_only_function<Event>&& handler) override;
  void AsyncRead(std::move_only_function<ReadHandler>&& handler) override;
  void AsyncWrite(const boost::asio::const_buffer& buffer, std::move_only_function<WriteHandler>&& handler);

private:
  Exec _E;
};

} // namespace gh
