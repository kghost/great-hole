#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <utility>

namespace udp_dyn_mux {

// Message Types
enum class msg_type : uint8_t {
  client_req_id = 0x01,
  server_assign_id = 0x02,
  server_keepalive = 0x03,
  client_keepalive_ack = 0x04,
  server_id_closed = 0x05,
  server_addr_mismatch = 0x06,
  client_addr_migrate = 0x07,
  server_migrate_ack = 0x08,
  server_invalid_id = 0x09,
  server_cookie_mismatch = 0x0A
};

inline bool operator==(uint8_t a, msg_type b) { return a == std::to_underlying(b); }
inline bool operator==(msg_type a, uint8_t b) { return std::to_underlying(a) == b; }

// Helper for endian conversions
inline uint16_t read_uint16_be(const uint8_t* data) { return (static_cast<uint16_t>(data[0]) << 8) | data[1]; }

inline void write_uint16_be(uint8_t* data, uint16_t val) {
  data[0] = (val >> 8) & 0xFF;
  data[1] = val & 0xFF;
}

inline uint32_t read_uint32_be(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

inline void write_uint32_be(uint8_t* data, uint32_t val) {
  data[0] = (val >> 24) & 0xFF;
  data[1] = (val >> 16) & 0xFF;
  data[2] = (val >> 8) & 0xFF;
  data[3] = val & 0xFF;
}

// Structures representing each control message type

struct client_req_id {
  static constexpr msg_type type = msg_type::client_req_id;
  static constexpr size_t size = 7;

  uint32_t cookie;

  static std::optional<client_req_id> deserialize(const uint8_t* data, size_t len) {
    if (len < size || read_uint16_be(data) != 0 || data[2] != static_cast<uint8_t>(type)) {
      return std::nullopt;
    }
    return client_req_id{read_uint32_be(data + 3)};
  }

  void serialize(std::span<uint8_t> out) const {
    assert(out.size() >= size);
    write_uint16_be(out.data(), 0);
    out[2] = static_cast<uint8_t>(type);
    write_uint32_be(out.data() + 3, cookie);
  }
};

struct server_assign_id {
  static constexpr msg_type type = msg_type::server_assign_id;
  static constexpr size_t size = 9;

  uint32_t cookie;
  uint16_t assigned_id;

  static std::optional<server_assign_id> deserialize(const uint8_t* data, size_t len) {
    if (len < size || read_uint16_be(data) != 0 || data[2] != static_cast<uint8_t>(type)) {
      return std::nullopt;
    }
    return server_assign_id{read_uint32_be(data + 3), read_uint16_be(data + 7)};
  }

  void serialize(std::span<uint8_t> out) const {
    assert(out.size() >= size);
    write_uint16_be(out.data(), 0);
    out[2] = static_cast<uint8_t>(type);
    write_uint32_be(out.data() + 3, cookie);
    write_uint16_be(out.data() + 7, assigned_id);
  }
};

// Common layout for 5-byte packets containing message type and a 16-bit ID
template <msg_type T> struct control_id_packet {
  static constexpr msg_type type = T;
  static constexpr size_t size = 5;

  uint16_t id;

  static std::optional<control_id_packet> deserialize(const uint8_t* data, size_t len) {
    if (len < size || read_uint16_be(data) != 0 || data[2] != static_cast<uint8_t>(type)) {
      return std::nullopt;
    }
    return control_id_packet{read_uint16_be(data + 3)};
  }

  void serialize(std::span<uint8_t> out) const {
    assert(out.size() >= size);
    write_uint16_be(out.data(), 0);
    out[2] = static_cast<uint8_t>(type);
    write_uint16_be(out.data() + 3, id);
  }
};

using server_keepalive = control_id_packet<msg_type::server_keepalive>;
using client_keepalive_ack = control_id_packet<msg_type::client_keepalive_ack>;
using server_id_closed = control_id_packet<msg_type::server_id_closed>;
using server_addr_mismatch = control_id_packet<msg_type::server_addr_mismatch>;
using server_migrate_ack = control_id_packet<msg_type::server_migrate_ack>;
using server_invalid_id = control_id_packet<msg_type::server_invalid_id>;
using server_cookie_mismatch = control_id_packet<msg_type::server_cookie_mismatch>;

struct client_addr_migrate {
  static constexpr msg_type type = msg_type::client_addr_migrate;
  static constexpr size_t size = 9;

  uint16_t id;
  uint32_t cookie;

  static std::optional<client_addr_migrate> deserialize(const uint8_t* data, size_t len) {
    if (len < size || read_uint16_be(data) != 0 || data[2] != static_cast<uint8_t>(type)) {
      return std::nullopt;
    }
    return client_addr_migrate{read_uint16_be(data + 3), read_uint32_be(data + 5)};
  }

  void serialize(std::span<uint8_t> out) const {
    assert(out.size() >= size);
    write_uint16_be(out.data(), 0);
    out[2] = static_cast<uint8_t>(type);
    write_uint16_be(out.data() + 3, id);
    write_uint32_be(out.data() + 5, cookie);
  }
};

} // namespace udp_dyn_mux
