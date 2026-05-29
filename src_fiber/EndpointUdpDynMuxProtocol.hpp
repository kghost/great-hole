#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <utility>

namespace gh::UdpDynMux {

// Message Types
enum class MsgType : uint8_t {
  kClientReqId = 0x01,
  kServerAssignId = 0x02,
  kServerKeepalive = 0x03,
  kClientKeepaliveAck = 0x04,
  kServerIdClosed = 0x05,
  kServerAddrMismatch = 0x06,
  kClientAddrMigrate = 0x07,
  kServerMigrateAck = 0x08,
  kServerInvalidId = 0x09,
  kServerCookieMismatch = 0x0A
};

inline bool operator==(uint8_t a, MsgType b) { return a == std::to_underlying(b); }
inline bool operator==(MsgType a, uint8_t b) { return std::to_underlying(a) == b; }

// Helper for endian conversions
inline uint16_t ReadUint16Be(const uint8_t* data) { return (static_cast<uint16_t>(data[0]) << 8) | data[1]; }

inline void WriteUint16Be(uint8_t* data, uint16_t val) {
  data[0] = (val >> 8) & 0xFF;
  data[1] = val & 0xFF;
}

inline uint32_t ReadUint32Be(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

inline void WriteUint32Be(uint8_t* data, uint32_t val) {
  data[0] = (val >> 24) & 0xFF;
  data[1] = (val >> 16) & 0xFF;
  data[2] = (val >> 8) & 0xFF;
  data[3] = val & 0xFF;
}

// Structures representing each control message type

struct ClientReqId {
  static constexpr MsgType kType = MsgType::kClientReqId;
  static constexpr size_t kSize = 7;

  uint32_t Cookie;

  static std::optional<ClientReqId> Deserialize(const uint8_t* data, size_t len) {
    if (len < kSize || ReadUint16Be(data) != 0 || data[2] != static_cast<uint8_t>(kType)) {
      return std::nullopt;
    }
    return ClientReqId{ReadUint32Be(data + 3)};
  }

  void Serialize(std::span<uint8_t> out) const {
    assert(out.size() >= kSize);
    WriteUint16Be(out.data(), 0);
    out[2] = static_cast<uint8_t>(kType);
    WriteUint32Be(out.data() + 3, Cookie);
  }
};

struct ServerAssignId {
  static constexpr MsgType kType = MsgType::kServerAssignId;
  static constexpr size_t kSize = 9;

  uint32_t Cookie;
  uint16_t AssignedId;

  static std::optional<ServerAssignId> Deserialize(const uint8_t* data, size_t len) {
    if (len < kSize || ReadUint16Be(data) != 0 || data[2] != static_cast<uint8_t>(kType)) {
      return std::nullopt;
    }
    return ServerAssignId{ReadUint32Be(data + 3), ReadUint16Be(data + 7)};
  }

  void Serialize(std::span<uint8_t> out) const {
    assert(out.size() >= kSize);
    WriteUint16Be(out.data(), 0);
    out[2] = static_cast<uint8_t>(kType);
    WriteUint32Be(out.data() + 3, Cookie);
    WriteUint16Be(out.data() + 7, AssignedId);
  }
};

// Common layout for 5-byte packets containing message type and a 16-bit ID
template <MsgType T> struct ControlIdPacket {
  static constexpr MsgType kType = T;
  static constexpr size_t kSize = 5;

  uint16_t Id;

  static std::optional<ControlIdPacket> Deserialize(const uint8_t* data, size_t len) {
    if (len < kSize || ReadUint16Be(data) != 0 || data[2] != static_cast<uint8_t>(kType)) {
      return std::nullopt;
    }
    return ControlIdPacket{ReadUint16Be(data + 3)};
  }

  void Serialize(std::span<uint8_t> out) const {
    assert(out.size() >= kSize);
    WriteUint16Be(out.data(), 0);
    out[2] = static_cast<uint8_t>(kType);
    WriteUint16Be(out.data() + 3, Id);
  }
};

using ServerKeepalive = ControlIdPacket<MsgType::kServerKeepalive>;
using ClientKeepaliveAck = ControlIdPacket<MsgType::kClientKeepaliveAck>;
using ServerIdClosed = ControlIdPacket<MsgType::kServerIdClosed>;
using ServerAddrMismatch = ControlIdPacket<MsgType::kServerAddrMismatch>;
using ServerMigrateAck = ControlIdPacket<MsgType::kServerMigrateAck>;
using ServerInvalidId = ControlIdPacket<MsgType::kServerInvalidId>;
using ServerCookieMismatch = ControlIdPacket<MsgType::kServerCookieMismatch>;

struct ClientAddrMigrate {
  static constexpr MsgType kType = MsgType::kClientAddrMigrate;
  static constexpr size_t kSize = 9;

  uint16_t Id;
  uint32_t Cookie;

  static std::optional<ClientAddrMigrate> Deserialize(const uint8_t* data, size_t len) {
    if (len < kSize || ReadUint16Be(data) != 0 || data[2] != static_cast<uint8_t>(kType)) {
      return std::nullopt;
    }
    return ClientAddrMigrate{ReadUint16Be(data + 3), ReadUint32Be(data + 5)};
  }

  void Serialize(std::span<uint8_t> out) const {
    assert(out.size() >= kSize);
    WriteUint16Be(out.data(), 0);
    out[2] = static_cast<uint8_t>(kType);
    WriteUint16Be(out.data() + 3, Id);
    WriteUint32Be(out.data() + 5, Cookie);
  }
};

} // namespace gh::UdpDynMux
