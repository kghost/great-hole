#pragma once

#include <vector>

#include "filter.hpp"
#include "packet.hpp"

namespace gh {

class FilterXor : public FilterSymmetric<FilterXor> {
public:
  explicit FilterXor(const std::vector<char>& key) : _Key(key) {}

  Packet Pipe(Packet&& p) override {
    auto& buffer = p.first;
    auto data = buffer.Data;

    for (std::size_t i = 0; i < buffer.Length; ++i) {
      data[buffer.Offset + i] ^= _Key[i % _Key.size()];
    }
    return std::move(p);
  }

private:
  std::vector<char> _Key;
};

} // namespace gh
