#pragma once

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "error-code.hpp"
#include "packet.hpp"

namespace gh {

class EndpointInput {
public:
  virtual ~EndpointInput() = 0;
  virtual Omni::Fiber::Coroutine<ErrorCode> Start() = 0;
  virtual Omni::Fiber::Coroutine<ErrorCode> Stop() = 0;
  virtual Omni::Fiber::Coroutine<ErrorCode> Read(Packet&, Cancel&) = 0;
};

class EndpointOutput {
public:
  virtual ~EndpointOutput() = 0;
  virtual Omni::Fiber::Coroutine<ErrorCode> Start() = 0;
  virtual Omni::Fiber::Coroutine<ErrorCode> Stop() = 0;
  virtual Omni::Fiber::Coroutine<ErrorCode> Write(Packet&, Cancel&) = 0;
};

class Endpoint : public EndpointInput, public EndpointOutput {
public:
  virtual Omni::Fiber::Coroutine<ErrorCode> Start() = 0;
};

template <typename Base> class EndpointSkipStart : public Base {
public:
  Omni::Fiber::Coroutine<ErrorCode> Start() override { co_return ErrorCode(); }
  Omni::Fiber::Coroutine<ErrorCode> Stop() override { co_return ErrorCode(); }
};

} // namespace gh
