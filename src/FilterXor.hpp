#pragma once

#include <vector>

#include "Filter.hpp"
#include "Packet.hpp"

namespace gh {

class FilterXor : public Filter {
public:
  explicit FilterXor(const std::vector<char>& key) : _Key(key) {}

  Omni::Fiber::Coroutine<boost::system::error_code> Pipe(Packet& p, Cancel& c) override {
    for (std::size_t i = 0; i < p._Length; ++i) {
      p._Data.data()[p._Offset + i] ^= _Key[i % _Key.size()];
    }
    co_return boost::system::error_code{};
  }

private:
  std::vector<char> _Key;
};

} // namespace gh
