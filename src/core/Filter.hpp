#pragma once

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "Packet.hpp"

namespace gh {

class Filter {
public:
  virtual ~Filter() = 0;
  virtual auto Pipe(Packet& p, Cancel&) -> Omni::Fiber::Coroutine<std::error_code> = 0;
};

} // namespace gh
