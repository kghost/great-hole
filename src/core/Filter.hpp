#pragma once

#include <boost/system/detail/error_code.hpp>

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "Packet.hpp"

namespace gh {

class Filter {
public:
  virtual ~Filter() = 0;
  virtual auto Pipe(Packet& p, Cancel&) -> Omni::Fiber::Coroutine<boost::system::error_code> = 0;
};

} // namespace gh
