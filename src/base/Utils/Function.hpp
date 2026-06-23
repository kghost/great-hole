#pragma once

#include <tuple>

namespace gh {

template <typename T> struct FunctionTraits;

template <typename R, typename... Args> struct FunctionTraits<R(Args...)> {
  using Return = R;
  using ArgsTuple = std::tuple<Args...>;
  template <std::size_t I> using Arg = std::tuple_element_t<I, ArgsTuple>;
};

template <typename R, typename... Args> struct FunctionTraits<R(Args...) noexcept> {
  using Return = R;
  using ArgsTuple = std::tuple<Args...>;
  template <std::size_t I> using Arg = std::tuple_element_t<I, ArgsTuple>;
};

} // namespace gh
