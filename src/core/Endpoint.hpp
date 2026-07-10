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
  auto GetPipielineUsageCounter() -> PipielineUsageCounter& { return _PipielineUsageCounter; }

protected:
  PipielineUsageCounter _PipielineUsageCounter;
};

class EndpointInput {
public:
  virtual ~EndpointInput() = 0;
  virtual auto Read(Packet&, Cancel&) -> Omni::Fiber::Coroutine<ErrorCode> = 0;
};

class EndpointOutput {
public:
  virtual ~EndpointOutput() = 0;
  virtual auto Write(Packet&, Cancel&) -> Omni::Fiber::Coroutine<ErrorCode> = 0;
};

class Endpoint : public EndpointMixin, public EndpointInput, public EndpointOutput {
public:
  virtual ~Endpoint() = default;
};

} // namespace gh
