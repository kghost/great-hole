#pragma once

#include <functional>

namespace gh {

template <typename T>
  requires(!std::is_pointer_v<T> && !std::is_reference_v<T>)
struct Less {
  using is_transparent = void;
  bool operator()(const T& lhs, const T& rhs) const { return &lhs < &rhs; }
  bool operator()(std::reference_wrapper<T> lhs, std::reference_wrapper<T> rhs) const {
    return &lhs.get() < &rhs.get();
  }
  bool operator()(std::reference_wrapper<T> lhs, const T& rhs) const { return &lhs.get() < &rhs; }
  bool operator()(const T& lhs, std::reference_wrapper<T> rhs) const { return &lhs < &rhs.get(); }
};

} // namespace gh
