#pragma once

#include "Coroutine.hpp"
#include "ErrorCode.hpp"

namespace gh {

class Service {
public:
  virtual ~Service() = default;

  virtual Omni::Fiber::Coroutine<ErrorCode> Start() = 0;
  virtual Omni::Fiber::Coroutine<void> Stop() = 0;
};

} // namespace gh
