#pragma once

#include <boost/any.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/core/noncopyable.hpp>

#if BOOST_VERSION < 106600
#define mutable_buffer mutable_buffers_1
#define const_buffer const_buffers_1
#endif // BOOST_VERSION  < 106600

namespace gh {

class Buffer {
public:
  static constexpr const std::size_t kReservedFront = 2;

  template <std::size_t N> explicit Buffer(std::array<uint8_t, N>& storage) {
    Data = storage.data();
    Capacity = storage.size();
    Offset = kReservedFront;
    Length = 0;
  }

  explicit Buffer(std::string& s) {
    Data = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(s.data()));
    Capacity = s.size();
    Offset = 0;
    Length = s.size();
  }

  Buffer(Buffer&&) = default;
  Buffer& operator=(Buffer&&) = default;
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

  explicit operator boost::asio::const_buffer() { return {Data + Offset, Length}; }

  explicit operator boost::asio::mutable_buffer() { return {Data + Offset, Capacity - Offset}; }

  //  data
  //   | reserved_front |    data    | unused back |
  //
  //   |<-   offset   ->|<- length ->|
  //   |<-                 capacity              ->|
  //
  //
  uint8_t* Data;
  std::size_t Capacity;
  std::size_t Offset;
  std::size_t Length;
};

// Packet.second stores a object which holds the owner of Buffer
using Packet = std::pair<Buffer, boost::any>;

} // namespace gh
