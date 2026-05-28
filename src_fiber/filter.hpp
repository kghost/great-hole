#pragma once

#include <boost/system/detail/error_code.hpp>

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "packet.hpp"

namespace gh {

class Filter {
public:
  virtual ~Filter() = 0;
  virtual Omni::Fiber::Coroutine<boost::system::error_code> Pipe(Packet& p, Cancel&) = 0;
};

} // namespace gh
