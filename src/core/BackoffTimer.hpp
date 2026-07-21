#pragma once

#include <chrono>

#if defined(__has_include) && __has_include(<generator>)
#include <generator>
#else
#include "Utils/Generator.hpp"
#endif

namespace gh {

template <typename Generator> class GeneratorHelper {
public:
  using ValueType = std::ranges::range_value_t<Generator>;
  using IteratorType = std::ranges::iterator_t<Generator>;

  explicit GeneratorHelper(Generator generator) : _Generator(std::move(generator)), _Iterator(_Generator.begin()) {}

  auto operator()() -> ValueType {
    auto value = std::move(*_Iterator);
    ++_Iterator;
    return value;
  }
  [[nodiscard]] auto hasNext() const -> bool { return _Iterator != _Generator.end(); }

private:
  Generator _Generator;
  IteratorType _Iterator;
};

auto BackoffTimerDuration(int randomnessPercent, std::chrono::milliseconds start, std::chrono::milliseconds step,
                          std::chrono::milliseconds maximum)
    -> GeneratorHelper<std::generator<std::chrono::milliseconds>>;

} // namespace gh