#pragma once

#include "Coroutine.hpp"
#include "ErrorCode.hpp"

namespace gh {

class Service {
public:
  explicit Service() = default;
  virtual ~Service() = default;

  Service(const Service&) = delete;
  auto operator=(const Service&) -> Service& = delete;
  Service(Service&&) = delete;
  auto operator=(Service&&) -> Service& = delete;

  virtual auto Start() -> Omni::Fiber::Coroutine<ErrorCode> = 0;
  virtual auto Stop() -> Omni::Fiber::Coroutine<ErrorCode> = 0;
};

} // namespace gh
