#pragma once

#include <stdexcept>
#include <utility>

#include "Coroutine.hpp"

namespace gh {

template <auto Max> void ToConst(decltype(Max) index, auto&& func) {
  bool matched = ([]<auto... Is>(decltype(Max) index, auto&& func, std::index_sequence<Is...> /*seq*/) -> bool {
    return ((index == Is ? (func.template operator()<Is>(), true) : false) || ...);
  })(index, std::forward<decltype(func)>(func), std::make_index_sequence<Max>{});

  if (!matched) {
    throw std::out_of_range("Runtime index exceeds the specified Max compile-time limit.");
  }
}

namespace detail {

template <auto Max, typename Func, decltype(Max)... Is>
auto DispatchConstAsync(decltype(Max) index, std::index_sequence<Is...> /*is*/)
    -> Omni::Fiber::Coroutine<void> (*)(Func&) {
  using Invoker = Omni::Fiber::Coroutine<void> (*)(Func&);
  static const auto kTable = std::array<Invoker, Max>{
      [](Func& func) -> Omni::Fiber::Coroutine<void> { co_await func.template operator()<Is>(); }...};
  return kTable[index];
}

} // namespace detail

template <auto Max> auto ToConstAsync(decltype(Max) index, auto&& func) -> Omni::Fiber::Coroutine<void> {
  if constexpr (std::is_signed_v<decltype(Max)>) {
    if (index < 0) {
      throw std::out_of_range("Runtime index exceeds the specified Max compile-time limit.");
    }
  }
  if (index >= Max) {
    throw std::out_of_range("Runtime index exceeds the specified Max compile-time limit.");
  }

  co_await detail::DispatchConstAsync<Max, decltype(func)>(index, std::make_index_sequence<Max>{})(func);
}

} // namespace gh
