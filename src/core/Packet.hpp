#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

#ifndef __cpp_lib_byteswap
namespace std {
template <typename T> constexpr T byteswap(T value) noexcept {
  static_assert(std::is_integral_v<T>, "std::byteswap is only for integral types");
  if constexpr (sizeof(T) == 1) {
    return value;
  } else if constexpr (sizeof(T) == 2) {
    return __builtin_bswap16(static_cast<uint16_t>(value));
  } else if constexpr (sizeof(T) == 4) {
    return __builtin_bswap32(static_cast<uint32_t>(value));
  } else if constexpr (sizeof(T) == 8) {
    return __builtin_bswap64(static_cast<uint64_t>(value));
  }
}
} // namespace std
#endif

#include <boost/asio/buffer.hpp>

namespace gh {

class Packet final {
public:
  static constexpr const std::size_t kCapacity = 2048;
  static constexpr const std::size_t kReservedFront = 2;

  explicit Packet(const std::size_t length = kCapacity, const std::size_t offset = kReservedFront)
      : _Data(length + offset), _Offset(offset), _Length(length) {}
  ~Packet() {}

  Packet(const Packet&) = delete;
  Packet& operator=(const Packet&) = delete;
  Packet(Packet&&) = default;
  Packet& operator=(Packet&&) = default;

  operator boost::asio::const_buffer() const { return {_Data.data() + _Offset, _Length}; }
  operator boost::asio::mutable_buffer() { return {_Data.data() + _Offset, _Length}; }

  std::size_t DataSize() const { return _Length; }
  std::size_t FrontSpace() const { return _Offset; }
  std::size_t BackSpace() const { return _Data.capacity() - _Offset - _Length; }

  std::span<uint8_t> Data() { return std::span<uint8_t>(_Data.data() + _Offset, _Length); }
  std::span<const uint8_t> Data() const { return std::span<const uint8_t>(_Data.data() + _Offset, _Length); }

  template <typename T>
    requires std::is_integral_v<T>
  void PushFront(T value) {
    T v = (std::endian::native == std::endian::little) ? std::byteswap(value) : value;
    PushFront(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&v), sizeof(v)));
  }

  void PushFront(std::span<const uint8_t> data) {
    assert(FrontSpace() >= data.size());
    _Offset -= data.size();
    _Length += data.size();
    std::copy(data.begin(), data.end(), _Data.data() + _Offset);
  }

  template <typename T>
    requires std::is_integral_v<T>
  T PopFront() {
    T v = *reinterpret_cast<T*>(PopFront<sizeof(T)>().data());
    return (std::endian::native == std::endian::little) ? std::byteswap(v) : v;
  }

  template <size_t Size> std::span<uint8_t, Size> PopFront() {
    assert(DataSize() >= Size);
    auto span = std::span<uint8_t, Size>(_Data.data() + _Offset, Size);
    _Offset += Size;
    _Length -= Size;
    return span;
  }

  template <typename T>
    requires std::is_integral_v<T>
  void PushBack(T value) {
    T v = (std::endian::native == std::endian::little) ? std::byteswap(value) : value;
    PushBack(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&v), sizeof(v)));
  }

  void PushBack(std::span<const uint8_t> data) {
    assert(BackSpace() >= data.size());
    std::copy(data.begin(), data.end(), _Data.data() + _Offset + _Length);
    _Length += data.size();
  }

  template <typename T>
    requires std::is_integral_v<T>
  T PopBack() {
    T v = *reinterpret_cast<T*>(PopBack<sizeof(T)>().data());
    return (std::endian::native == std::endian::little) ? std::byteswap(v) : v;
  }

  template <size_t Size> std::span<uint8_t, Size> PopBack() {
    assert(DataSize() >= Size);
    _Length -= Size;
    return std::span<uint8_t, Size>(_Data.data() + _Offset + _Length);
  }

  //  data
  //   | reserved_front |    data    | unused back |
  //
  //   |<-   offset   ->|<- length ->|
  //   |<-                 capacity              ->|
  //
  //
  std::vector<uint8_t> _Data;
  std::size_t _Offset;
  std::size_t _Length;
};

} // namespace gh
