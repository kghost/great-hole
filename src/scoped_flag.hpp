#pragma once

#include <cassert>
#include <functional>
#include <optional>

namespace gh {

class ScopedFlag {
public:
  explicit ScopedFlag(bool& flag) : _Flag(flag) {
    assert(!_Flag.value().get());
    _Flag.value().get() = true;
  }
  ~ScopedFlag() {
    if (_Flag) {
      assert(_Flag.value().get());
      _Flag.value().get() = false;
    }
  }

  ScopedFlag(const ScopedFlag&) = delete;
  ScopedFlag& operator=(const ScopedFlag&) = delete;

  ScopedFlag(ScopedFlag&& other) noexcept { _Flag.swap(other._Flag); }

  ScopedFlag& operator=(ScopedFlag&& other) noexcept {
    if (_Flag) {
      assert(_Flag.value().get());
      _Flag.value().get() = false;
      _Flag.reset();
    }
    _Flag.swap(other._Flag);
    return *this;
  }

private:
  std::optional<std::reference_wrapper<bool>> _Flag;
};

} // namespace gh
