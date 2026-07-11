#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

#include <boost/asio/buffer.hpp>

#include "Utils/Endian.hpp"

namespace gh {

class PacketMark {
public:
  explicit PacketMark() = default;
  virtual ~PacketMark() = default;

  PacketMark(const PacketMark&) = delete;
  auto operator=(const PacketMark&) -> PacketMark& = delete;
  PacketMark(PacketMark&&) = delete;
  auto operator=(PacketMark&&) -> PacketMark& = delete;
};

class Packet final {
public:
  static constexpr const std::size_t kCapacity = 2048;
  static constexpr const std::size_t kReservedFront = 2;

  explicit Packet(const std::size_t length = kCapacity, const std::size_t offset = kReservedFront)
      : _Data(length + offset), _Offset(offset), _Length(length) {}
  ~Packet() {}

  Packet(const Packet&) = delete;
  auto operator=(const Packet&) -> Packet& = delete;
  Packet(Packet&&) = default;
  auto operator=(Packet&&) -> Packet& = default;

  void SetMark(std::unique_ptr<PacketMark> mark) const { _Mark = std::move(mark); }
  [[nodiscard]] auto HasMark() const -> bool { return _Mark != nullptr; }
  [[nodiscard]] auto GetMark() const -> PacketMark& { return *_Mark; }

  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  operator boost::asio::const_buffer() const { return {_Data.data() + _Offset, _Length}; }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  operator boost::asio::mutable_buffer() { return {_Data.data() + _Offset, _Length}; }

  [[nodiscard]] auto DataSize() const -> std::size_t { return _Length; }
  [[nodiscard]] auto FrontSpace() const -> std::size_t { return _Offset; }
  [[nodiscard]] auto BackSpace() const -> std::size_t { return _Data.capacity() - _Offset - _Length; }

  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  auto Data() -> std::span<uint8_t> { return {_Data.data() + _Offset, _Length}; }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  [[nodiscard]] auto Data() const -> std::span<const uint8_t> { return {_Data.data() + _Offset, _Length}; }

  template <typename T>
    requires std::is_integral_v<T>
  void PushFront(T value) {
    value = ArchEndian(value);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    PushFront(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&value), sizeof(value)));
  }

  void PushFront(std::span<const uint8_t> data) {
    assert(FrontSpace() >= data.size());
    _Offset -= data.size();
    _Length += data.size();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    std::ranges::copy(data, _Data.data() + _Offset);
  }

  template <typename T>
    requires std::is_integral_v<T>
  auto PopFront() -> T {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return ArchEndian(*reinterpret_cast<T*>(PopFront<sizeof(T)>().data()));
  }

  template <size_t Size> auto PopFront() -> std::span<uint8_t, Size> {
    assert(DataSize() >= Size);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto span = std::span<uint8_t, Size>(_Data.data() + _Offset, Size);
    _Offset += Size;
    _Length -= Size;
    return span;
  }

  template <typename T>
    requires std::is_integral_v<T>
  void PushBack(T value) {
    value = ArchEndian(value);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    PushBack(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&value), sizeof(value)));
  }

  void PushBack(std::span<const uint8_t> data) {
    assert(BackSpace() >= data.size());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    std::ranges::copy(data, _Data.data() + _Offset + _Length);
    _Length += data.size();
  }

  template <typename T>
    requires std::is_integral_v<T>
  auto PopBack() -> T {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return ArchEndian(*reinterpret_cast<T*>(PopBack<sizeof(T)>().data()));
  }

  template <size_t Size> auto PopBack() -> std::span<uint8_t, Size> {
    assert(DataSize() >= Size);
    _Length -= Size;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return std::span<uint8_t, Size>(_Data.data() + _Offset + _Length);
  }

  //  data
  //   | reserved_front |    data    | unused back |
  //
  //   |<-   offset   ->|<- length ->|
  //   |<-              _Data.capacity           ->|
  std::vector<uint8_t> _Data;
  std::size_t _Offset;
  std::size_t _Length;
  mutable std::unique_ptr<PacketMark> _Mark = nullptr;
};

} // namespace gh
