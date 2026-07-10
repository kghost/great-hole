#pragma once

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "Utils/All.hpp"
#include "Utils/Endian.hpp"
#include "Utils/ToC.hpp"

namespace gh {

// ---------------------------------- Concepts ---------------------------------- //
template <typename T>
concept PacketField = requires {
  typename T::TargetType;
  requires std::same_as<decltype(T::Size), const size_t> || std::same_as<decltype(T::Size), size_t>;
  { T::Validate(std::declval<std::span<const uint8_t>>()) } -> std::same_as<bool>;
  { T::Get(std::declval<std::span<const uint8_t, T::Size>>()) } -> std::same_as<typename T::TargetType>;
  { T::Set(std::declval<std::span<uint8_t, T::Size>>(), std::declval<typename T::TargetType>()) } -> std::same_as<void>;
};

template <typename T> struct CanSetWith {
  template <typename ArgsTuple> struct Test : std::false_type {};

  template <typename... Args> struct Test<std::tuple<Args...>> {
    static constexpr bool value =
        requires { T::Set(std::declval<std::span<uint8_t, T::BuilderDataSize>>(), std::declval<Args>()...); };
  };
};

template <typename T>
concept PacketComponent = requires {
  { T::Validate(std::declval<std::span<const uint8_t>>()) } -> std::same_as<bool>;
  requires std::same_as<decltype(T::BuilderDataSize), const size_t>;
  typename T::BuilderArgumentsList;
  requires All<CanSetWith<T>::template Test, typename T::BuilderArgumentsList>::value;
};

// ---------------------------------- PacketComponentEnd ---------------------------------- //
class PacketComponentEnd {
public:
  static constexpr size_t BuilderDataSize = 0;
  using BuilderArgumentsList = std::tuple<>;
  static auto Validate(std::span<const uint8_t> /*data*/) -> bool { return true; }
};

// ---------------------------------- Builder / Parser ---------------------------------- //
template <typename Target, size_t Offset>
  requires PacketComponent<Target>
class PacketBuilder {
public:
  explicit PacketBuilder(std::span<uint8_t> data) : _Data(data) {}

  auto operator()(auto&&... args) {
    auto ret =
        Target::Set(_Data.template subspan<Offset, Target::BuilderDataSize>(), std::forward<decltype(args)>(args)...);
    return PacketBuilder<decltype(ret), Offset + Target::BuilderDataSize>(_Data);
  }

private:
  std::span<uint8_t> _Data;
};

template <typename Result> class ParseEnd {
public:
  ParseEnd(auto&&... args) : _Result(std::forward<decltype(args)>(args)...) {}
  auto Value() -> Result { return std::move(_Result); }

private:
  Result _Result;
};

template <> class ParseEnd<void> {
public:
  ParseEnd() = default;
};

template <typename Result, typename Target, size_t Offset>
  requires PacketComponent<Target>
class PacketParser {
private:
  struct NextParserCaller {
    std::span<const uint8_t> Data;

    template <typename NextComponent>
      requires(!std::same_as<NextComponent, PacketComponentEnd>)
    auto operator()(auto&& next) const -> ParseEnd<Result> {
      return PacketParser<Result, NextComponent, Offset + Target::BuilderDataSize>{Data}(
          std::forward<decltype(next)>(next));
    }

    template <typename NextComponent>
      requires(!std::same_as<NextComponent, PacketComponentEnd>)
    auto operator()(NextComponent /*unused*/, auto&& next) const -> ParseEnd<Result> {
      return PacketParser<Result, NextComponent, Offset + Target::BuilderDataSize>{Data}(
          std::forward<decltype(next)>(next));
    }

    template <typename ComponentEnd, typename ResultType>
      requires std::same_as<ComponentEnd, PacketComponentEnd>
    auto operator()(ComponentEnd /*unused*/, ResultType result) const -> ParseEnd<Result> {
      return result;
    }

    template <typename ComponentEnd, typename ResultType>
      requires std::same_as<ComponentEnd, PacketComponentEnd>
    auto operator()(ResultType result) const -> ParseEnd<Result> {
      return result;
    }

    template <typename ComponentEnd> auto operator()() const -> ParseEnd<Result> { return {}; }
  };

public:
  explicit PacketParser(std::span<const uint8_t> data) : _Data(data) {}

  auto operator()(auto&& func) const -> ParseEnd<Result> {
    return Target::template Parse<Result>(_Data.template subspan<Offset, Target::BuilderDataSize>(),
                                          std::forward<decltype(func)>(func), NextParserCaller{_Data});
  }

private:
  std::span<const uint8_t> _Data;
};

// ---------------------------------- Predefined Field Types ---------------------------------- //
template <typename EnumType>
  requires(std::is_scoped_enum_v<EnumType>)
class PacketFieldEnum {
public:
  using TargetType = EnumType;
  static constexpr const size_t Size = sizeof(EnumType);
  static auto Validate(std::span<const uint8_t> data) -> bool { return data.size() >= Size; }
  static auto Get(std::span<const uint8_t, Size> data) -> EnumType {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return static_cast<EnumType>(ArchEndian(*reinterpret_cast<const std::underlying_type_t<EnumType>*>(data.data())));
  }
  static void Set(std::span<uint8_t, Size> data, EnumType val) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    *reinterpret_cast<std::underlying_type_t<EnumType>*>(data.data()) =
        ArchEndian(static_cast<std::underlying_type_t<EnumType>>(val));
  }
};

template <typename Numeric>
  requires(std::is_fundamental_v<Numeric>)
class PacketFieldNumeric {
public:
  using TargetType = Numeric;
  static constexpr const size_t Size = sizeof(Numeric);
  static auto Validate(std::span<const uint8_t> data) -> bool { return data.size() >= Size; }
  static auto Get(std::span<const uint8_t, Size> data) -> Numeric {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return static_cast<Numeric>(ArchEndian(*reinterpret_cast<const Numeric*>(data.data())));
  }
  static void Set(std::span<uint8_t, Size> data, Numeric val) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    *reinterpret_cast<Numeric*>(data.data()) = ArchEndian(val);
  }
};

template <size_t Length> class PacketFieldRaw {
public:
  using TargetType = std::span<const uint8_t, Length>;
  static constexpr const size_t Size = Length;
  static auto Validate(std::span<const uint8_t> data) -> bool { return data.size() >= Size; }
  static auto Get(std::span<const uint8_t, Size> data) -> std::span<const uint8_t, Size> { return data; }
  static void Set(std::span<uint8_t, Size> data, std::span<const uint8_t, Size> val) {
    std::ranges::copy(val, data.begin());
  }
};

template <typename NameTag, typename UnderlyingType> class PacketFieldAlias {
public:
  static_assert(PacketField<UnderlyingType>, "UnderlyingType must satisfy PacketField!");
  using TargetType = typename UnderlyingType::TargetType;
  static constexpr const size_t Size = UnderlyingType::Size;
  static auto Validate(std::span<const uint8_t> data) -> bool { return UnderlyingType::Validate(data); }
  static auto Get(std::span<const uint8_t, Size> data) -> TargetType { return UnderlyingType::Get(data); }
  static void Set(std::span<uint8_t, Size> data, TargetType val) { UnderlyingType::Set(data, val); }
};

// ---------------------------------- PacketComponentContainer ---------------------------------- //
template <typename FieldsTuple, typename NextComponent> class PacketComponentContainer;

template <typename... Fields, typename NextComponent>
  requires(PacketField<Fields> && ...)
class PacketComponentContainer<std::tuple<Fields...>, NextComponent> {
public:
  using FieldsTuple = std::tuple<Fields...>;

  struct FieldInfo {
    size_t Offset = 0;
    size_t Size = 0;
  };

  static constexpr const std::array<FieldInfo, sizeof...(Fields)> FieldInfos =
      []() consteval->std::array<FieldInfo, sizeof...(Fields)> {
    std::array<FieldInfo, sizeof...(Fields)> result{};
    auto process = [&]<std::size_t Is, typename Field>() -> auto {
      result[Is].Size = Field::Size;
      if constexpr (Is == 0) {
        result[Is].Offset = 0;
      } else {
        result[Is].Offset = result[Is - 1].Offset + result[Is - 1].Size;
      }
    };
    [&]<std::size_t... Is>(std::index_sequence<Is...>) -> auto {
      (process.template operator()<Is, Fields>(), ...);
    }(std::make_index_sequence<sizeof...(Fields)>{});
    return result;
  }
  ();

  static constexpr const size_t Size = []() consteval -> size_t {
    if constexpr (sizeof...(Fields) == 0) {
      return 0;
    } else {
      return FieldInfos[sizeof...(Fields) - 1].Offset + FieldInfos[sizeof...(Fields) - 1].Size;
    }
  }();

  using BuilderArgumentsList = std::tuple<std::tuple<typename Fields::TargetType...>>;
  static constexpr size_t BuilderDataSize = Size;

  static auto Validate(std::span<const uint8_t> data) -> bool {
    return (data.size() >= Size) && [&]<std::size_t... Is>(std::index_sequence<Is...>) -> bool {
      return (Fields::Validate(data.template subspan<FieldInfos[Is].Offset, FieldInfos[Is].Size>()) && ...);
    }(std::make_index_sequence<sizeof...(Fields)>{});
  }

  static auto Set(std::span<uint8_t, BuilderDataSize> data, typename Fields::TargetType... args) {
    [&]<std::size_t... Is>(std::index_sequence<Is...>) -> auto {
      ((Fields::Set(data.template subspan<FieldInfos[Is].Offset, FieldInfos[Is].Size>(), args)), ...);
    }(std::make_index_sequence<sizeof...(Fields)>{});
    return NextComponent{};
  }

  template <typename RetType>
  static auto Parse(std::span<const uint8_t, BuilderDataSize> data, auto&& func, auto&& next) -> ParseEnd<RetType> {
    return [&]<std::size_t... Is>(std::index_sequence<Is...>) -> ParseEnd<RetType> {
      using ResultType =
          decltype(func(Fields::Get(data.template subspan<FieldInfos[Is].Offset, FieldInfos[Is].Size>())...));
      if constexpr (std::is_void_v<ResultType>) {
        func(Fields::Get(data.template subspan<FieldInfos[Is].Offset, FieldInfos[Is].Size>())...);
        return next.template operator()<NextComponent>();
      } else {
        return next.template operator()<NextComponent, ResultType>(
            func(Fields::Get(data.template subspan<FieldInfos[Is].Offset, FieldInfos[Is].Size>())...));
      }
    }(std::make_index_sequence<sizeof...(Fields)>{});
  }
};

// ---------------------------------- PacketComponentEnumMap ---------------------------------- //
class PacketComponentEnumMapParseFailedPacket {
public:
  static constexpr size_t BuilderDataSize = 0;
  using BuilderArgumentsList = std::tuple<>;
  static auto Validate(std::span<const uint8_t> /*data*/) -> bool { return false; }
  template <typename RetType>
  static auto Parse(std::span<const uint8_t, BuilderDataSize> /*data*/, ParseEnd<RetType>&& end, auto&& /*next*/)
      -> ParseEnd<RetType> {
    return std::move(end);
  }
};

template <auto EnumValue, typename TargetPacketComponent>
  requires PacketComponent<TargetPacketComponent>
struct PacketComponentEnumMapEntry {
  static constexpr auto Value = EnumValue;
  using EnumType = decltype(EnumValue);
  using Type = TargetPacketComponent;
  using IsPacketComponentEnumMapEntry = std::true_type;
};

template <typename T>
concept IsPacketComponentEnumMapEntryType = requires { typename T::IsPacketComponentEnumMapEntry; };

template <typename MapTuple, typename FallbackComponent = PacketComponentEnumMapParseFailedPacket>
class PacketComponentEnumMap;

template <typename... Entries, typename FallbackComponent>
  requires(IsPacketComponentEnumMapEntryType<Entries> && ...) && PacketComponent<FallbackComponent>
class PacketComponentEnumMap<std::tuple<Entries...>, FallbackComponent> {
public:
  using FirstType = std::tuple_element_t<0, std::tuple<Entries...>>;
  using EnumType = typename FirstType::EnumType;
  using EnumField = PacketFieldEnum<EnumType>;

  static_assert((std::is_same_v<EnumType, typename Entries::EnumType> && ...),
                "All entries must have the same enum type");
  static_assert((PacketComponent<typename Entries::Type> && ...), "All entries must be PacketComponent");

  using BuilderArgumentsList = std::tuple<std::tuple<std::integral_constant<EnumType, Entries::Value>>...>;
  static constexpr size_t BuilderDataSize = sizeof(EnumType);

  static auto Validate(std::span<const uint8_t> data) -> bool {
    if (!EnumField::Validate(data)) {
      return false;
    }
    return std::visit(
        [&]<typename ComponentType>(ComponentType /*component*/) -> auto {
          return ComponentType::Validate(data.template subspan<EnumField::Size>());
        },
        EnumToVariant(EnumField::Get(std::span<const uint8_t, EnumField::Size>(data.data(), EnumField::Size))));
  }

private:
  template <EnumType value, typename... SetEntries> struct SetResult;

  template <EnumType value, typename FirstEntry, typename... RestEntries>
  struct SetResult<value, FirstEntry, RestEntries...> {
    static auto operator()() {
      if constexpr (value == FirstEntry::Value) {
        return typename FirstEntry::Type{};
      } else {
        return SetResult<value, RestEntries...>::operator()();
      }
    }
  };

  template <EnumType value> struct SetResult<value> {
    static auto operator()() { return FallbackComponent{}; }
  };

public:
  template <EnumType value>
  static auto Set(std::span<uint8_t, BuilderDataSize> data, std::integral_constant<EnumType, value> /*unused*/) {
    EnumField::Set(data.template subspan<0, EnumField::Size>(), value);
    return SetResult<value, Entries...>::operator()();
  }

private:
  template <typename RetType, typename... ParseEntries> struct ParseResult;

  template <typename RetType, typename FirstEntry, typename... RestEntries>
  struct ParseResult<RetType, FirstEntry, RestEntries...> {
    static auto operator()(EnumType value, auto&& overloaded, auto&& next) -> ParseEnd<RetType> {
      if (value == FirstEntry::Value) {
        return next(typename FirstEntry::Type{}, overloaded(ToC<FirstEntry::Value>()));
      } else {
        return ParseResult<RetType, RestEntries...>::operator()(value, std::forward<decltype(overloaded)>(overloaded),
                                                                std::forward<decltype(next)>(next));
      }
    }
  };

  template <typename RetType> struct ParseResult<RetType> {
    static auto operator()(EnumType value, auto&& overloaded, auto&& next) -> ParseEnd<RetType> {
      return next(FallbackComponent{}, [&]() -> ParseEnd<RetType> {
        if constexpr (std::is_void_v<decltype(overloaded(value))>) {
          overloaded(value);
          return {};
        } else {
          return {overloaded(value)};
        }
      }());
    }
  };

public:
  template <typename RetType>
  static auto Parse(std::span<const uint8_t, BuilderDataSize> data, auto&& overloaded, auto&& next)
      -> ParseEnd<RetType> {
    return ParseResult<RetType, Entries...>::operator()(EnumField::Get(data.template subspan<0, EnumField::Size>()),
                                                        std::forward<decltype(overloaded)>(overloaded),
                                                        std::forward<decltype(next)>(next));
  }

private:
  using Variant = std::variant<FallbackComponent, typename Entries::Type...>;

  static constexpr auto EnumToVariant(EnumType value) -> Variant {
    Variant result = FallbackComponent{};
    [&]<std::size_t... Is>(std::index_sequence<Is...>) -> auto {
      auto test = [&]<std::size_t I, typename Entry>() -> auto {
        if (Entry::Value == value) {
          result.template emplace<I + 1>(typename Entry::Type{});
        }
      };
      (test.template operator()<Is, Entries>(), ...);
    }(std::make_index_sequence<sizeof...(Entries)>{});
    return result;
  }
};

} // namespace gh
