#pragma once

#include "Coroutine.hpp"
#include "Event.hpp"
#include "error-code.hpp"
#include "packet.hpp"

namespace gh {

using Event = void(ErrorCode const&);
using ReadHandler = void(ErrorCode const&, Packet&&);
using WriteHandler = void(ErrorCode const&, std::size_t);

class EndpointInput {
public:
  virtual ~EndpointInput() = 0;
  virtual Omni::Fiber::Coroutine<ErrorCode> Start(Omni::Fiber::Event<>&) = 0;
  virtual Omni::Fiber::Coroutine<std::tuple<ErrorCode, Packet>> Read() = 0;
};

class EndpointOutput {
public:
  virtual ~EndpointOutput() = 0;
  virtual Omni::Fiber::Coroutine<ErrorCode> Start(Omni::Fiber::Event<>&) = 0;
  virtual Omni::Fiber::Coroutine<std::tuple<ErrorCode, std::size_t>> Write(Packet&&) = 0;
};

class Endpoint : public EndpointInput, public EndpointOutput {};

template <typename Base> class EndpointSkipStart : public Base {
public:
  Omni::Fiber::Coroutine<ErrorCode> Start(Omni::Fiber::Event<>&) override { co_return ErrorCode(); }
};

} // namespace gh
