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

constexpr uint8_t kMajorVersion = 1;
constexpr uint8_t kMinorVersion = 0;
constexpr uint16_t kPatchVersion = 0;

// Message Types
enum class MsgType : uint8_t {
  kInitiate = 0x01,
  kInitiateFail = 0x02,
  kKeepalive = 0x03,
  kInvalidPsk = 0x09,
  kInvalidChannel = 0x0a,
  kInvalidAddress = 0x0b
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

inline void WritePsk(uint8_t* data, const PskType& psk) { std::copy(psk.begin(), psk.end(), data); }

// Structures representing symmetric control messages

struct Initiate {
  static constexpr MsgType kType = MsgType::kInitiate;
  static constexpr size_t kSize = 27;

  PskType Psk;
  uint16_t RxId;
  uint16_t PeerRxId;
  uint8_t Major;
  uint8_t Minor;
  uint16_t Patch;

  static std::optional<Initiate> Deserialize(std::span<const uint8_t> data) {
    if (data.size() < kSize || ReadUint16Be(data.data()) != 0 || data[2] != static_cast<uint8_t>(kType)) {
      return std::nullopt;
    }
    return Initiate{
        ReadPsk(data.data() + 3),
        ReadUint16Be(data.data() + 19),
        ReadUint16Be(data.data() + 21),
        data[23],
        data[24],
        ReadUint16Be(data.data() + 25)};
  }

  void Serialize(std::span<uint8_t> out) const {
    assert(out.size() >= kSize);
    WriteUint16Be(out.data(), 0);
    out[2] = static_cast<uint8_t>(kType);
    WritePsk(out.data() + 3, Psk);
    WriteUint16Be(out.data() + 19, RxId);
    WriteUint16Be(out.data() + 21, PeerRxId);
    out[23] = Major;
    out[24] = Minor;
    WriteUint16Be(out.data() + 25, Patch);
  }
};

struct InvalidChannel {
  static constexpr MsgType kType = MsgType::kInvalidChannel;
  static constexpr size_t kSize = 5;

  uint16_t ChannelId;

  static std::optional<InvalidChannel> Deserialize(std::span<const uint8_t> data) {
    if (data.size() < kSize || ReadUint16Be(data.data()) != 0 || data[2] != static_cast<uint8_t>(kType)) {
      return std::nullopt;
    }
    return InvalidChannel{ReadUint16Be(data.data() + 3)};
  }

  void Serialize(std::span<uint8_t> out) const {
    assert(out.size() >= kSize);
    WriteUint16Be(out.data(), 0);
    out[2] = static_cast<uint8_t>(kType);
    WriteUint16Be(out.data() + 3, ChannelId);
  }
};

struct InvalidAddress {
  static constexpr MsgType kType = MsgType::kInvalidAddress;
  static constexpr size_t kSize = 5;

  uint16_t ChannelId;

  static std::optional<InvalidAddress> Deserialize(std::span<const uint8_t> data) {
    if (data.size() < kSize || ReadUint16Be(data.data()) != 0 || data[2] != static_cast<uint8_t>(kType)) {
      return std::nullopt;
    }
    return InvalidAddress{ReadUint16Be(data.data() + 3)};
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

  static std::optional<ControlPskPacket> Deserialize(std::span<const uint8_t> data) {
    if (data.size() < kSize || ReadUint16Be(data.data()) != 0 || data[2] != static_cast<uint8_t>(kType)) {
      return std::nullopt;
    }
    return ControlPskPacket{ReadPsk(data.data() + 3)};
  }

  void Serialize(std::span<uint8_t> out) const {
    assert(out.size() >= kSize);
    WriteUint16Be(out.data(), 0);
    out[2] = static_cast<uint8_t>(kType);
    WritePsk(out.data() + 3, Psk);
  }
};

struct Keepalive {
  static constexpr MsgType kType = MsgType::kKeepalive;
  static constexpr size_t kSize = 20;

  PskType Psk;
  uint8_t Flags;

  bool IsPing() const { return (Flags & 0x01) != 0; }
  bool IsPong() const { return (Flags & 0x02) != 0; }

  static std::optional<Keepalive> Deserialize(std::span<const uint8_t> data) {
    if (data.size() < kSize || ReadUint16Be(data.data()) != 0 || data[2] != static_cast<uint8_t>(kType)) {
      return std::nullopt;
    }
    return Keepalive{ReadPsk(data.data() + 3), data[19]};
  }

  void Serialize(std::span<uint8_t> out) const {
    assert(out.size() >= kSize);
    WriteUint16Be(out.data(), 0);
    out[2] = static_cast<uint8_t>(kType);
    WritePsk(out.data() + 3, Psk);
    out[19] = Flags;
  }
};

using InitiateFail = ControlPskPacket<MsgType::kInitiateFail>;
using InvalidPsk = ControlPskPacket<MsgType::kInvalidPsk>;

} // namespace gh::UdpDynMuxProto
