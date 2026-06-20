#pragma once

#include <type_traits>

namespace gh {

template <typename T, template <auto...> class Template> struct IsInstanceOf : std::false_type {};

template <template <auto...> class Template, auto... Args>
struct IsInstanceOf<Template<Args...>, Template> : std::true_type {};

template <typename T, template <auto...> class Template>
constexpr bool IsInstanceOfV = IsInstanceOf<T, Template>::value;

template <typename T, template <auto...> class Template>
concept InstanceOf = IsInstanceOfV<T, Template>;

} // namespace gh
