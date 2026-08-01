#pragma once

#include <array>
#include <bit>
#include <span>
#include <type_traits>

namespace gh {

template <typename ElementType, size_t Size>
auto ToArray(const std::span<ElementType, Size>& span) -> std::array<std::remove_cv_t<ElementType>, Size> {
  std::array<std::remove_cv_t<ElementType>, Size> result{};
  std::ranges::copy(span, result.begin());
  return result;
}

template <typename FieldType, size_t Offset> auto SpanToField(const std::span<const uint8_t>& span) -> FieldType {
  return std::bit_cast<FieldType>(ToArray(span.subspan<Offset, sizeof(FieldType)>()));
}
template <typename FieldType> auto SpanToField(const std::span<const uint8_t>& span, size_t offset) -> FieldType {
  return std::bit_cast<FieldType>(ToArray(span.subspan(offset).template subspan<0, sizeof(FieldType)>()));
}

template <typename TargetElementType, typename OriginElementType>
auto View(const std::span<OriginElementType>& span) -> std::span<TargetElementType> {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return std::span<TargetElementType>(reinterpret_cast<TargetElementType*>(span.data()),
                                      span.size() * sizeof(OriginElementType) / sizeof(TargetElementType));
}

} // namespace gh
