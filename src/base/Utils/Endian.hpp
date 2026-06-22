#pragma once

#include <bit>

namespace gh {

// ============================================================================
// ENDIAN CONVERSION UTILITY (C++23 Modern replacement for ntohs / ntohl)
// ============================================================================
template <typename T> [[nodiscard]] constexpr T ArchEndian(T value) noexcept {
  // Network Byte Order is strictly Big Endian.
  // If the host system is Little Endian, swap the bytes.
  if constexpr (std::endian::native == std::endian::little) {
    return std::byteswap(value); // C++23 intrinsic byte swapping
  }
  return value;
}

} // namespace gh
