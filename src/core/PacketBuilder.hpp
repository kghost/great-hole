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
#include "Utils/IndexOf.hpp"

namespace gh {

template <typename T>
concept PacketField = requires {
  typename T::TargetType;
  requires std::same_as<decltype(T::Size), const size_t> || std::same_as<decltype(T::Size), size_t>;
  { T::Validate(std::declval<std::span<const uint8_t>>()) } -> std::same_as<bool>;
  { T::Get(std::declval<std::span<const uint8_t, T::Size>>()) } -> std::same_as<typename T::TargetType>;
  { T::Set(std::declval<std::span<uint8_t, T::Size>>(), std::declval<typename T::TargetType>()) } -> std::same_as<void>;
};

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

template <typename Target, size_t Offset> class PacketBuilder;
template <typename Target, size_t Offset> class PacketParser;

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

class PacketComponentEnd {};

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

  static constexpr const std::array<FieldInfo, sizeof...(Fields)> FieldOffsets = []() consteval {
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
      return FieldOffsets[sizeof...(Fields) - 1].Offset + FieldOffsets[sizeof...(Fields) - 1].Size;
    }
  }();

  using BuilderArgumentsList = std::tuple<std::tuple<typename Fields::TargetType...>>;
  static constexpr size_t BuilderDataSize = Size;

  static bool Validate(std::span<const uint8_t> data) {
    return (data.size() >= Size) && [&]<std::size_t... Is>(std::index_sequence<Is...>) -> bool {
      return (Fields::Validate(data.template subspan<FieldOffsets[Is].Offset, FieldOffsets[Is].Size>()) && ...);
    }(std::make_index_sequence<sizeof...(Fields)>{});
  }

  static auto Set(std::span<uint8_t, BuilderDataSize> data, typename Fields::TargetType... args) {
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      ((Fields::Set(data.template subspan<FieldOffsets[Is].Offset, Fields::Size>(), args)), ...);
    }(std::make_index_sequence<sizeof...(Fields)>{});
    return NextComponent{};
  }
};

class PacketComponentEnumMapParseFailedPacket {
public:
  static bool Validate(std::span<const uint8_t> data) { return false; }
};

template <auto EnumValue, typename TargetPacketComponent>
  requires PacketComponent<TargetPacketComponent>
struct PacketComponentEnumMapEntry {
  static constexpr auto Value = EnumValue;
  using EnumType = decltype(EnumValue);
  using Type = TargetPacketComponent;
};

template <typename... Entries> class PacketComponentEnumMap {
public:
  using FirstType = std::tuple_element_t<0, std::tuple<Entries...>>;
  using EnumType = typename FirstType::EnumType;

  static_assert((std::is_same_v<EnumType, typename Entries::EnumType> && ...),
                "All entries must have the same enum type");
  static_assert((PacketComponent<typename Entries::Type> && ...), "All entries must be PacketComponent");

  using BuilderArgumentsList = std::tuple<std::tuple<std::integral_constant<EnumType, Entries::Value>>...>;
  static constexpr size_t BuilderDataSize = sizeof(EnumType);

  static bool Validate(std::span<const uint8_t> data) {
    using Enum = PacketFieldEnum<EnumType>;
    if (!Enum::Validate(data)) {
      return false;
    }
    return std::visit(
        [&]<typename ComponentType>(ComponentType component) {
          return ComponentType::Validate(data.template subspan<Enum::Size>());
        },
        EnumToVariant(Enum::Get(std::span<const uint8_t, Enum::Size>(data.data(), Enum::Size))));
  }

  template <EnumType value>
  static auto Set(std::span<uint8_t, BuilderDataSize> data, std::integral_constant<EnumType, value> v) {
    using Enum = PacketFieldEnum<EnumType>;
    Enum::Set(data.template subspan<0, Enum::Size>(), v);

    auto ret = [&]<typename FirstEntry, typename... RestEntries>(this auto&& self) -> auto {
      if constexpr (value == FirstEntry::Value) {
        return typename FirstEntry::Type{};
      } else {
        return self.template operator()<RestEntries...>();
      }
    };
    return ret.template operator()<Entries...>();
  }

  template <typename EntryComponent, size_t Offset>
  static std::optional<PacketParser<EntryComponent, Offset + PacketFieldEnum<EnumType>::Size>>
  As(std::span<const uint8_t> data) {
    using Enum = PacketFieldEnum<EnumType>;
    if (data.size() < Offset + Enum::Size) {
      return std::nullopt;
    }
    auto activeEnum = Enum::Get(data.subspan<Offset, Enum::Size>());

    bool match = false;
    auto check = [&]<typename Entry>() {
      if constexpr (std::is_same_v<typename Entry::Type, EntryComponent>) {
        if (Entry::Value == activeEnum) {
          match = true;
        }
      }
    };
    (check.template operator()<Entries>(), ...);

    if (match) {
      return PacketParser<EntryComponent, Offset + Enum::Size>(data);
    }
    return std::nullopt;
  }

private:
  using Variant = std::variant<PacketComponentEnumMapParseFailedPacket, typename Entries::Type...>;

  static constexpr Variant EnumToVariant(EnumType type) {
    Variant result = PacketComponentEnumMapParseFailedPacket{};
    auto test = [&]<typename Entry>() {
      if (Entry::Value == type) {
        result = typename Entry::Type{};
      }
    };
    (test.template operator()<Entries>(), ...);
    return result;
  }
};

template <typename Target, size_t Offset> class PacketBuilder {
public:
  PacketBuilder(std::span<uint8_t> data) : _Data(data) {}

  template <typename... Args> auto operator()(Args&&... args) {
    auto ret = Target::Set(_Data.template subspan<Offset, Target::BuilderDataSize>(), std::forward<Args>(args)...);
    return PacketBuilder<decltype(ret), Offset + Target::BuilderDataSize>(_Data);
  }

private:
  std::span<uint8_t> _Data;
};

template <size_t Offset> class PacketBuilder<PacketComponentEnd, Offset> {
public:
  PacketBuilder(std::span<uint8_t> data) {}
};

template <typename Target, size_t Offset> class PacketParser {
public:
  PacketParser(std::span<const uint8_t> data) : _Data(data) {}

  template <PacketField Field> typename Field::TargetType Get() const {
    using FieldsTuple = typename Target::FieldsTuple;
    constexpr int idx = IndexInTuple<Field, FieldsTuple>::value;
    static_assert(idx != -1, "Field not found in this PacketComponent!");

    constexpr size_t fieldOffset = Target::FieldOffsets[idx].Offset;
    constexpr size_t fieldSize = Target::FieldOffsets[idx].Size;
    return Field::Get(_Data.template subspan<Offset + fieldOffset, fieldSize>());
  }

  template <typename EntryComponent> auto As() const { return Target::template As<EntryComponent, Offset>(_Data); }

private:
  std::span<const uint8_t> _Data;
};

} // namespace gh
