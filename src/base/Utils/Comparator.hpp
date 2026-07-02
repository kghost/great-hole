#pragma once

#include <memory>

namespace gh {

struct SharedPtrCompare {
  using is_transparent = void;
  template <typename T> bool operator()(const std::shared_ptr<T>& lhs, const std::shared_ptr<T>& rhs) const {
    return lhs.get() < rhs.get();
  }
  template <typename T> bool operator()(const std::shared_ptr<T>& lhs, const T* rhs) const { return lhs.get() < rhs; }
  template <typename T> bool operator()(const T* lhs, const std::shared_ptr<T>& rhs) const { return lhs < rhs.get(); }
};

} // namespace gh
