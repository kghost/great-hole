#pragma once

#include <functional>

#include "error-code.hpp"
#include "packet.hpp"

namespace gh {

using Event = void(ErrorCode const&);
using ReadHandler = void(ErrorCode const&, Packet&&);
using WriteHandler = void(ErrorCode const&, std::size_t);

class EndpointInput {
public:
  virtual ~EndpointInput() = 0;
  virtual void AsyncStart(std::move_only_function<Event>&&) = 0;
  virtual void AsyncRead(std::move_only_function<ReadHandler>&&) = 0;
};

class EndpointOutput {
public:
  virtual ~EndpointOutput() = 0;
  virtual void AsyncStart(std::move_only_function<Event>&&) = 0;
  virtual void AsyncWrite(Packet&&, std::move_only_function<WriteHandler>&&) = 0;
};

class Endpoint : public EndpointInput, public EndpointOutput {};

template <typename Base> class EndpointSkipStart : public Base {
public:
  void AsyncStart(std::move_only_function<Event>&& handler) override { handler(ErrorCode()); }
};

} // namespace gh
