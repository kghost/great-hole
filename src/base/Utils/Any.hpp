#pragma once

#include <tuple>
#include <type_traits>

namespace gh {

template <template <typename...> class Test, typename Tuple> struct Any;

template <template <typename...> class Test, typename... Ts>
struct Any<Test, std::tuple<Ts...>> : std::bool_constant<(Test<Ts>::value || ...)> {};

} // namespace gh
