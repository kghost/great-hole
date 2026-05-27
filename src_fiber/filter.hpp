#pragma once

#include <boost/system/detail/error_code.hpp>

#include "Coroutine.hpp"
#include "packet.hpp"

namespace gh {

class Filter {
public:
  virtual ~Filter() = 0;
  virtual Omni::Fiber::Coroutine<std::tuple<boost::system::error_code, Packet>> Pipe(Packet&& p) = 0;
};

} // namespace gh
