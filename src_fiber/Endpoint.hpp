#pragma once

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "Packet.hpp"
#include "PipielineUsageCounter.hpp"
#include "ServiceBase.hpp"

namespace gh {

class EndpointInput : virtual public Service {
public:
  virtual ~EndpointInput() = 0;
  virtual Omni::Fiber::Coroutine<ErrorCode> Read(Packet&, Cancel&) = 0;
  virtual PipielineUsageCounter& GetPipielineUsageCounter() = 0;
};

class EndpointOutput : virtual public Service {
public:
  virtual ~EndpointOutput() = 0;
  virtual Omni::Fiber::Coroutine<ErrorCode> Write(Packet&, Cancel&) = 0;
  virtual PipielineUsageCounter& GetPipielineUsageCounter() = 0;
};

class Endpoint : virtual public ServiceBase, public EndpointInput, public EndpointOutput {
public:
  virtual ~Endpoint() = default;

  PipielineUsageCounter& GetPipielineUsageCounter() override { return _PipielineUsageCounter; }

protected:
  PipielineUsageCounter _PipielineUsageCounter;
};

template <typename Base> class EndpointSkipStart : public Base {
public:
  Omni::Fiber::Coroutine<ErrorCode> Start() override { co_return ErrorCode(); }
  Omni::Fiber::Coroutine<ErrorCode> Stop() override { co_return ErrorCode(); }
  PipielineUsageCounter& GetPipielineUsageCounter() override { return _PipielineUsageCounter; }

protected:
  PipielineUsageCounter _PipielineUsageCounter;
};

} // namespace gh
