#pragma once

#include <boost/asio/buffer.hpp>

namespace gh {

class Packet {
public:
  static constexpr const std::size_t kCapacity = 2048;
  static constexpr const std::size_t kReservedFront = 2;

  explicit Packet() : _Data(kCapacity, 0), _Offset(kReservedFront), _Length(0) {}
  explicit Packet(const std::size_t length) : _Data(length), _Offset(0), _Length(length) {}

  Packet(const Packet&) = delete;
  Packet& operator=(const Packet&) = delete;
  Packet(Packet&&) = default;
  Packet& operator=(Packet&&) = default;

  explicit operator boost::asio::const_buffer() const { return {_Data.data() + _Offset, _Length}; }
  explicit operator boost::asio::mutable_buffer() { return {_Data.data() + _Offset, _Data.size() - _Offset}; }

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
