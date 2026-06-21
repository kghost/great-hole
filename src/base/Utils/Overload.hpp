#pragma once

namespace gh {

template <class... Ts> struct Overload : Ts... {
  using Ts::operator()...;
};

} // namespace gh
