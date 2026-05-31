#include "BackoffTimer.hpp"

#include <random>

namespace gh {

GeneratorHelper<std::generator<std::chrono::milliseconds>> BackoffTimerDuration(int randomnessPercent,
                                                                                std::chrono::milliseconds current,
                                                                                std::chrono::milliseconds step,
                                                                                std::chrono::milliseconds maximum) {
  return GeneratorHelper(([](int randomnessPercent, std::chrono::milliseconds current, std::chrono::milliseconds step,
                             std::chrono::milliseconds maximum) -> std::generator<std::chrono::milliseconds> {
    std::mt19937 gen(std::random_device{}());
    while (true) {
      auto c = current.count();
      auto max = c * (100 + randomnessPercent) / 100;
      std::uniform_int_distribution<long long> dist(c, max);

      co_yield std::chrono::milliseconds(dist(gen));

      if (current < maximum) {
        current += step;

        if (current > maximum) {
          current = maximum;
        }
      }
    }
  })(randomnessPercent, current, step, maximum));
}

} // namespace gh