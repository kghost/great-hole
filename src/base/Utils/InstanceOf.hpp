#pragma once

#include <type_traits>

namespace gh {

template <typename T, template <typename...> class Template> struct IsInstanceOf : std::false_type {};

template <template <typename...> class Template, typename... Args>
struct IsInstanceOf<Template<Args...>, Template> : std::true_type {};

template <typename T, template <typename...> class Template>
constexpr bool IsInstanceOfV = IsInstanceOf<T, Template>::value;

template <typename T, template <typename...> class Template>
concept InstanceOf = IsInstanceOfV<T, Template>;

template <typename T, template <auto...> class Template> struct IsInstanceOfVal : std::false_type {};

template <template <auto...> class Template, auto... Args>
struct IsInstanceOfVal<Template<Args...>, Template> : std::true_type {};

template <typename T, template <auto...> class Template>
constexpr bool IsInstanceOfValV = IsInstanceOfVal<T, Template>::value;

template <typename T, template <auto...> class Template>
concept InstanceOfVal = IsInstanceOfValV<T, Template>;

template <typename T, template <auto, typename> class Template> struct IsInstanceOfValType : std::false_type {};

template <template <auto, typename> class Template, auto Arg1, typename Arg2>
struct IsInstanceOfValType<Template<Arg1, Arg2>, Template> : std::true_type {};

template <typename T, template <auto, typename> class Template>
constexpr bool IsInstanceOfValTypeV = IsInstanceOfValType<T, Template>::value;

template <typename T, template <auto, typename> class Template>
concept InstanceOfValType = IsInstanceOfValTypeV<T, Template>;

} // namespace gh
