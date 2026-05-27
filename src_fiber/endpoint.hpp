#pragma once

#include "Coroutine.hpp"
#include "Event.hpp"
#include "error-code.hpp"
#include "packet.hpp"

namespace gh {

class EndpointInput {
public:
  virtual ~EndpointInput() = 0;
  virtual Omni::Fiber::Coroutine<ErrorCode> Start(Omni::Fiber::Event<>&) = 0;
  virtual Omni::Fiber::Coroutine<ErrorCode> Read(Packet&) = 0;
};

class EndpointOutput {
public:
  virtual ~EndpointOutput() = 0;
  virtual Omni::Fiber::Coroutine<ErrorCode> Start(Omni::Fiber::Event<>&) = 0;
  virtual Omni::Fiber::Coroutine<ErrorCode> Write(Packet&) = 0;
};

class Endpoint : public EndpointInput, public EndpointOutput {};

template <typename Base> class EndpointSkipStart : public Base {
public:
  Omni::Fiber::Coroutine<ErrorCode> Start(Omni::Fiber::Event<>&) override { co_return ErrorCode(); }
};

} // namespace gh
