#pragma once

#include <algorithm>
#include <bit>
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
  static bool Validate(std::span<const uint8_t> data) { return true; }
};

// ---------------------------------- Builder / Parser ---------------------------------- //
template <typename Target, size_t Offset>
  requires PacketComponent<Target>
class PacketBuilder {
public:
  explicit PacketBuilder(std::span<uint8_t> data) : _Data(data) {}

  template <typename... Args> auto operator()(Args&&... args) {
    auto ret = Target::Set(_Data.template subspan<Offset, Target::BuilderDataSize>(), std::forward<Args>(args)...);
    return PacketBuilder<decltype(ret), Offset + Target::BuilderDataSize>(_Data);
  }

private:
  std::span<uint8_t> _Data;
};

template <typename Result> class ParseEnd {
public:
  template <typename... Args> ParseEnd(Args&&... args) : _Result(std::forward<Args>(args)...) {}
  Result Value() { return std::move(_Result); }

private:
  Result _Result;
};

template <> class ParseEnd<void> {
public:
  ParseEnd() {}
};

template <typename Result, typename Target, size_t Offset>
  requires PacketComponent<Target>
class PacketParser {
private:
  struct NextParserCaller {
    std::span<const uint8_t> Data;

    template <typename NextComponent, typename NextUserParseFunc>
      requires(!std::same_as<NextComponent, PacketComponentEnd>)
    auto operator()(NextUserParseFunc&& nextUserParseFunc) const -> ParseEnd<Result> {
      return PacketParser<Result, NextComponent, Offset + Target::BuilderDataSize>{Data}
          .template operator()<decltype(nextUserParseFunc)>(std::forward<NextUserParseFunc>(nextUserParseFunc));
    }

    template <typename NextComponent, typename NextUserParseFunc>
      requires(!std::same_as<NextComponent, PacketComponentEnd>)
    auto operator()(NextComponent, NextUserParseFunc&& nextUserParseFunc) const -> ParseEnd<Result> {
      return PacketParser<Result, NextComponent, Offset + Target::BuilderDataSize>{Data}
          .template operator()<decltype(nextUserParseFunc)>(std::forward<NextUserParseFunc>(nextUserParseFunc));
    }

    template <typename ComponentEnd, typename ResultType>
      requires std::same_as<ComponentEnd, PacketComponentEnd>
    auto operator()(ComponentEnd, ResultType result) const -> ParseEnd<Result> {
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

  template <typename Function> auto operator()(Function&& func) const -> ParseEnd<Result> {
    return Target::template Parse<Result>(_Data.template subspan<Offset, Target::BuilderDataSize>(),
                                          std::forward<Function>(func), NextParserCaller{_Data});
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
  static bool Validate(std::span<const uint8_t> data) { return data.size() >= Size; }
  static EnumType Get(std::span<const uint8_t, Size> data) {
    return static_cast<EnumType>(
        std::byteswap(*reinterpret_cast<const std::underlying_type_t<EnumType>*>(data.data())));
  }
  static void Set(std::span<uint8_t, Size> data, EnumType val) {
    *reinterpret_cast<std::underlying_type_t<EnumType>*>(data.data()) =
        std::byteswap(static_cast<std::underlying_type_t<EnumType>>(val));
  }
};

template <typename Numeric>
  requires(std::is_fundamental_v<Numeric>)
class PacketFieldNumeric {
public:
  using TargetType = Numeric;
  static constexpr const size_t Size = sizeof(Numeric);
  static bool Validate(std::span<const uint8_t> data) { return data.size() >= Size; }
  static Numeric Get(std::span<const uint8_t, Size> data) {
    return static_cast<Numeric>(std::byteswap(*reinterpret_cast<const Numeric*>(data.data())));
  }
  static void Set(std::span<uint8_t, Size> data, Numeric val) {
    *reinterpret_cast<Numeric*>(data.data()) = std::byteswap(val);
  }
};

template <size_t Length> class PacketFieldRaw {
public:
  using TargetType = std::span<const uint8_t, Length>;
  static constexpr const size_t Size = Length;
  static bool Validate(std::span<const uint8_t> data) { return data.size() >= Size; }
  static std::span<const uint8_t, Size> Get(std::span<const uint8_t, Size> data) { return data; }
  static void Set(std::span<uint8_t, Size> data, std::span<const uint8_t, Size> val) {
    std::ranges::copy(val, data.begin());
  }
};

template <typename NameTag, typename UnderlyingType> class PacketFieldAlias {
public:
  static_assert(PacketField<UnderlyingType>, "UnderlyingType must satisfy PacketField!");
  using TargetType = typename UnderlyingType::TargetType;
  static constexpr const size_t Size = UnderlyingType::Size;
  static bool Validate(std::span<const uint8_t> data) { return UnderlyingType::Validate(data); }
  static TargetType Get(std::span<const uint8_t, Size> data) { return UnderlyingType::Get(data); }
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

  static constexpr const std::array<FieldInfo, sizeof...(Fields)> FieldInfos = []() consteval {
    std::array<FieldInfo, sizeof...(Fields)> result{};
    auto process = [&]<std::size_t Is, typename Field>() {
      result[Is].Size = Field::Size;
      if constexpr (Is == 0) {
        result[Is].Offset = 0;
      } else {
        result[Is].Offset = result[Is - 1].Offset + result[Is - 1].Size;
      }
    };
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      (process.template operator()<Is, Fields>(), ...);
    }(std::make_index_sequence<sizeof...(Fields)>{});
    return result;
  }();

  static constexpr const size_t Size = []() consteval {
    if constexpr (sizeof...(Fields) == 0) {
      return 0;
    } else {
      return FieldInfos[sizeof...(Fields) - 1].Offset + FieldInfos[sizeof...(Fields) - 1].Size;
    }
  }();

  using BuilderArgumentsList = std::tuple<std::tuple<typename Fields::TargetType...>>;
  static constexpr size_t BuilderDataSize = Size;

  static bool Validate(std::span<const uint8_t> data) {
    return (data.size() >= Size) && [&]<std::size_t... Is>(std::index_sequence<Is...>) -> bool {
      return (Fields::Validate(data.template subspan<FieldInfos[Is].Offset, FieldInfos[Is].Size>()) && ...);
    }(std::make_index_sequence<sizeof...(Fields)>{});
  }

  static auto Set(std::span<uint8_t, BuilderDataSize> data, typename Fields::TargetType... args) {
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      ((Fields::Set(data.template subspan<FieldInfos[Is].Offset, FieldInfos[Is].Size>(), args)), ...);
    }(std::make_index_sequence<sizeof...(Fields)>{});
    return NextComponent{};
  }

  template <typename RetType, typename Function, typename CallNextParse>
  static ParseEnd<RetType> Parse(std::span<const uint8_t, BuilderDataSize> data, Function&& func,
                                 CallNextParse&& next) {
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
  static bool Validate(std::span<const uint8_t> data) { return false; }
  template <typename RetType, typename CallNextParse>
  static ParseEnd<RetType> Parse(std::span<const uint8_t, BuilderDataSize> data, ParseEnd<RetType>&& end,
                                 CallNextParse&& next) {
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

  static bool Validate(std::span<const uint8_t> data) {
    if (!EnumField::Validate(data)) {
      return false;
    }
    return std::visit(
        [&]<typename ComponentType>(ComponentType component) {
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
  static auto Set(std::span<uint8_t, BuilderDataSize> data, std::integral_constant<EnumType, value> v) {
    EnumField::Set(data.template subspan<0, EnumField::Size>(), v);
    return SetResult<value, Entries...>::operator()();
  }

private:
  template <typename RetType, typename Overloaded, typename CallNextParse, typename... ParseEntries> struct ParseResult;

  template <typename RetType, typename Overloaded, typename CallNextParse, typename FirstEntry, typename... RestEntries>
  struct ParseResult<RetType, Overloaded, CallNextParse, FirstEntry, RestEntries...> {
    static ParseEnd<RetType> operator()(EnumType value, Overloaded&& overloaded, CallNextParse&& next) {
      if (value == FirstEntry::Value) {
        return next(typename FirstEntry::Type{}, overloaded(ToC<FirstEntry::Value>()));
      } else {
        return ParseResult<RetType, Overloaded, CallNextParse, RestEntries...>::operator()(
            value, std::forward<Overloaded>(overloaded), std::forward<CallNextParse>(next));
      }
    }
  };

  template <typename RetType, typename Overloaded, typename CallNextParse>
  struct ParseResult<RetType, Overloaded, CallNextParse> {
    static ParseEnd<RetType> operator()(EnumType value, Overloaded&& overloaded, CallNextParse&& next) {
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
  template <typename RetType, typename Overloaded, typename CallNextParse>
  static ParseEnd<RetType> Parse(std::span<const uint8_t, BuilderDataSize> data, Overloaded&& overloaded,
                                 CallNextParse&& next) {
    return ParseResult<RetType, Overloaded, CallNextParse, Entries...>::operator()(
        EnumField::Get(data.template subspan<0, EnumField::Size>()), std::forward<Overloaded>(overloaded),
        std::forward<CallNextParse>(next));
  }

private:
  using Variant = std::variant<FallbackComponent, typename Entries::Type...>;

  static constexpr Variant EnumToVariant(EnumType type) {
    Variant result = FallbackComponent{};
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      auto test = [&]<std::size_t I, typename Entry>() {
        if (Entry::Value == type) {
          result.template emplace<I + 1>(typename Entry::Type{});
        }
      };
      (test.template operator()<Is, Entries>(), ...);
    }(std::make_index_sequence<sizeof...(Entries)>{});
    return result;
  }
};

} // namespace gh
