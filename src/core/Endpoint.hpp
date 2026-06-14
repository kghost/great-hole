#pragma once

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "Packet.hpp"
#include "PipielineUsageCounter.hpp"
#include "ServiceBase.hpp"

namespace gh {

class EndpointMixin : public ServiceBase {
public:
  PipielineUsageCounter& GetPipielineUsageCounter() { return _PipielineUsageCounter; }

protected:
  PipielineUsageCounter _PipielineUsageCounter;
};

class EndpointInput {
public:
  virtual ~EndpointInput() = 0;
  virtual Omni::Fiber::Coroutine<ErrorCode> Read(Packet&, Cancel&) = 0;
};

class EndpointOutput {
public:
  virtual ~EndpointOutput() = 0;
  virtual Omni::Fiber::Coroutine<ErrorCode> Write(Packet&, Cancel&) = 0;
};

class Endpoint : public EndpointMixin, public EndpointInput, public EndpointOutput {
public:
  virtual ~Endpoint() = default;
};

} // namespace gh
