#pragma once

#include <vector>

#include "filter.hpp"
#include "packet.hpp"

namespace gh {

class FilterXor : public Filter {
public:
  explicit FilterXor(const std::vector<char>& key) : _Key(key) {}

  Omni::Fiber::Coroutine<boost::system::error_code> Pipe(Packet& p) override {
    for (std::size_t i = 0; i < p._Length; ++i) {
      p._Data.data()[p._Offset + i] ^= _Key[i % _Key.size()];
    }
    co_return boost::system::error_code{};
  }

private:
  std::vector<char> _Key;
};

} // namespace gh
