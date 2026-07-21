#include "BackoffTimer.hpp"

#include <random>

namespace gh {

auto BackoffTimerDuration(int randomnessPercent, std::chrono::milliseconds start, std::chrono::milliseconds step,
                          std::chrono::milliseconds maximum)
    -> GeneratorHelper<std::generator<std::chrono::milliseconds>> {
  return GeneratorHelper(([](int randomnessPercent, std::chrono::milliseconds current, std::chrono::milliseconds step,
                             std::chrono::milliseconds maximum) -> std::generator<std::chrono::milliseconds> {
    std::mt19937 gen(std::random_device{}());
    while (true) {
      auto currentCount = current.count();
      auto max = currentCount * (100 + randomnessPercent) / 100;
      std::uniform_int_distribution<long long> dist(currentCount, max);

      co_yield std::chrono::milliseconds(dist(gen));

      if (current < maximum) {
        current += step;

        if (current > maximum) {
          current = maximum;
        }
      }
    }
  })(randomnessPercent, start, step, maximum));
}

} // namespace gh