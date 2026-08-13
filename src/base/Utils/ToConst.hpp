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

template <auto Max> auto ToConstAsync(decltype(Max) index, auto&& func) -> Omni::Fiber::Coroutine<void> {
  bool matched = co_await ([]<auto... Is>(decltype(Max) index, auto&& func,
                                          std::index_sequence<Is...> /*seq*/) -> Omni::Fiber::Coroutine<bool> {
    co_return ((index == Is ? (co_await func.template operator()<Is>(), true) : false) || ...);
  })(index, std::forward<decltype(func)>(func), std::make_index_sequence<Max>{});

  if (!matched) {
    throw std::out_of_range("Runtime index exceeds the specified Max compile-time limit.");
  }

  co_return;
}

} // namespace gh
