#pragma once

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "error-code.hpp"
#include "packet.hpp"
#include "service.hpp"

namespace gh {

class EndpointInput : virtual public Service {
public:
  virtual ~EndpointInput() = 0;
  virtual Omni::Fiber::Coroutine<ErrorCode> Read(Packet&, Cancel&) = 0;
};

class EndpointOutput : virtual public Service {
public:
  virtual ~EndpointOutput() = 0;
  virtual Omni::Fiber::Coroutine<ErrorCode> Write(Packet&, Cancel&) = 0;
};

class Endpoint : public EndpointInput, public EndpointOutput {
public:
  virtual ~Endpoint() = default;
};

template <typename Base> class EndpointSkipStart : public Base {
public:
  Omni::Fiber::Coroutine<ErrorCode> Start() override { co_return ErrorCode(); }
  Omni::Fiber::Coroutine<ErrorCode> Stop() override { co_return ErrorCode(); }
};

} // namespace gh

