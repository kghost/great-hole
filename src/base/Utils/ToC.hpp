#pragma once

#include <type_traits>
namespace gh {

template <auto Value> consteval auto ToC() { return std::integral_constant<decltype(Value), Value>{}; }
template <auto Value> using C = std::integral_constant<decltype(Value), Value>;

} // namespace gh
