#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <utility>

namespace gh::UdpDynMuxProto {

// Message Types
enum class MsgType : uint8_t {
  kInitiate = 0x01,
  kKeepalive = 0x03,
  kKeepaliveAck = 0x04,
  kInvalidPsk = 0x09,
  kInvalidChannel = 0x0a
};

inline bool operator==(uint8_t a, MsgType b) { return a == std::to_underlying(b); }
inline bool operator==(MsgType a, uint8_t b) { return std::to_underlying(a) == b; }

// Helper for endian conversions
inline uint16_t ReadUint16Be(const uint8_t* data) { return (static_cast<uint16_t>(data[0]) << 8) | data[1]; }

inline void WriteUint16Be(uint8_t* data, uint16_t val) {
  data[0] = (val >> 8) & 0xFF;
  data[1] = val & 0xFF;
}

// 16-byte PSK type and helpers
using PskType = std::array<uint8_t, 16>;

inline PskType ReadPsk(const uint8_t* data) {
  PskType psk;
  std::copy_n(data, 16, psk.begin());
  return psk;
}

inline void WritePsk(uint8_t* data, const PskType& psk) {
  std::copy(psk.begin(), psk.end(), data);
}

// Structures representing symmetric control messages

struct Initiate {
  static constexpr MsgType kType = MsgType::kInitiate;
  static constexpr size_t kSize = 23;

  PskType Psk;
  uint16_t RxId;
  uint16_t PeerRxId;

  static std::optional<Initiate> Deserialize(const uint8_t* data, size_t len) {
    if (len < kSize || ReadUint16Be(data) != 0 || data[2] != static_cast<uint8_t>(kType)) {
      return std::nullopt;
    }
    return Initiate{ReadPsk(data + 3), ReadUint16Be(data + 19), ReadUint16Be(data + 21)};
  }

  void Serialize(std::span<uint8_t> out) const {
    assert(out.size() >= kSize);
    WriteUint16Be(out.data(), 0);
    out[2] = static_cast<uint8_t>(kType);
    WritePsk(out.data() + 3, Psk);
    WriteUint16Be(out.data() + 19, RxId);
    WriteUint16Be(out.data() + 21, PeerRxId);
  }
};

struct InvalidChannel {
  static constexpr MsgType kType = MsgType::kInvalidChannel;
  static constexpr size_t kSize = 5;

  uint16_t ChannelId;

  static std::optional<InvalidChannel> Deserialize(const uint8_t* data, size_t len) {
    if (len < kSize || ReadUint16Be(data) != 0 || data[2] != static_cast<uint8_t>(kType)) {
      return std::nullopt;
    }
    return InvalidChannel{ReadUint16Be(data + 3)};
  }

  void Serialize(std::span<uint8_t> out) const {
    assert(out.size() >= kSize);
    WriteUint16Be(out.data(), 0);
    out[2] = static_cast<uint8_t>(kType);
    WriteUint16Be(out.data() + 3, ChannelId);
  }
};

// Common layout for 19-byte packets containing message type and a 16-byte PSK
template <MsgType T> struct ControlPskPacket {
  static constexpr MsgType kType = T;
  static constexpr size_t kSize = 19;

  PskType Psk;

  static std::optional<ControlPskPacket> Deserialize(const uint8_t* data, size_t len) {
    if (len < kSize || ReadUint16Be(data) != 0 || data[2] != static_cast<uint8_t>(kType)) {
      return std::nullopt;
    }
    return ControlPskPacket{ReadPsk(data + 3)};
  }

  void Serialize(std::span<uint8_t> out) const {
    assert(out.size() >= kSize);
    WriteUint16Be(out.data(), 0);
    out[2] = static_cast<uint8_t>(kType);
    WritePsk(out.data() + 3, Psk);
  }
};

using Keepalive = ControlPskPacket<MsgType::kKeepalive>;
using KeepaliveAck = ControlPskPacket<MsgType::kKeepaliveAck>;
using InvalidPsk = ControlPskPacket<MsgType::kInvalidPsk>;

} // namespace gh::UdpDynMuxProto
