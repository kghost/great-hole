#pragma once

#include <chrono>
#include <generator>

namespace gh {

template <typename Generator> class GeneratorHelper {
public:
  using ValueType = std::ranges::range_value_t<Generator>;
  using IteratorType = std::ranges::iterator_t<Generator>;

  explicit GeneratorHelper(Generator generator) : _Generator(std::move(generator)), _Iterator(_Generator.begin()) {}

  ValueType operator()() {
    auto value = std::move(*_Iterator);
    ++_Iterator;
    return value;
  }
  bool hasNext() const { return _Iterator != _Generator.end(); }

private:
  Generator _Generator;
  IteratorType _Iterator;
};

GeneratorHelper<std::generator<std::chrono::milliseconds>> BackoffTimerDuration(int randomnessPercent,
                                                                                std::chrono::milliseconds current,
                                                                                std::chrono::milliseconds step,
                                                                                std::chrono::milliseconds maximum);

} // namespace gh