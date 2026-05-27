#pragma once

#include <vector>

#include "filter.hpp"
#include "packet.hpp"

namespace gh {

class FilterXor : public Filter {
public:
  explicit FilterXor(const std::vector<char>& key) : _Key(key) {}

  Omni::Fiber::Coroutine<std::tuple<boost::system::error_code, Packet>> Pipe(Packet&& p) override {
    auto& buffer = p.first;
    auto data = buffer.Data;

    for (std::size_t i = 0; i < buffer.Length; ++i) {
      data[buffer.Offset + i] ^= _Key[i % _Key.size()];
    }
    co_return std::tuple{boost::system::error_code(), std::move(p)};
  }

private:
  std::vector<char> _Key;
};

} // namespace gh
