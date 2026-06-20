#pragma once

#include <tuple>
#include <type_traits>

namespace gh {

template <typename T, typename... Ts> struct IndexOf;

template <typename T, typename First, typename... Rest> struct IndexOf<T, First, Rest...> {
  static constexpr int value =
      std::is_same_v<T, First> ? 0 : (IndexOf<T, Rest...>::value == -1 ? -1 : 1 + IndexOf<T, Rest...>::value);
};

template <typename T> struct IndexOf<T> {
  static constexpr int value = -1;
};

template <typename T, typename Tuple> struct IndexInTuple;

template <typename T, typename... Us> struct IndexInTuple<T, std::tuple<Us...>> {
  static constexpr int value = IndexOf<T, Us...>::value;
};

} // namespace gh
