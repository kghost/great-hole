#pragma once

#include <cstdint>

#include "PacketBuilder.hpp"

namespace gh::UdpDynMuxProto {

// NOLINTNEXTLINE(performance-enum-size)
enum class EnumChannel : uint16_t {
  kControlChannel = 0,
};

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

enum KeepaliveFlags : uint8_t { kPing = 0x01, kPong = 0x02 };

class TagChannel {};
class TagPsk {};
class TagRxId {};
class TagPeerRxId {};
class TagMajor {};
class TagMinor {};
class TagPatch {};
class TagKeepaliveFlag {};

using FieldChannel = PacketFieldAlias<TagChannel, PacketFieldNumeric<uint16_t>>;
using FieldPsk = PacketFieldAlias<TagPsk, PacketFieldRaw<16>>;
using FieldRxId = PacketFieldAlias<TagRxId, PacketFieldNumeric<uint16_t>>;
using FieldPeerRxId = PacketFieldAlias<TagPeerRxId, PacketFieldNumeric<uint16_t>>;
using FieldMajor = PacketFieldAlias<TagMajor, PacketFieldNumeric<uint8_t>>;
using FieldMinor = PacketFieldAlias<TagMinor, PacketFieldNumeric<uint8_t>>;
using FieldPatch = PacketFieldAlias<TagPatch, PacketFieldNumeric<uint16_t>>;
using FieldKeepaliveFlag = PacketFieldAlias<TagKeepaliveFlag, PacketFieldNumeric<uint8_t>>;

using PacketInitiate =
    PacketComponentContainer<std::tuple<FieldPsk, FieldRxId, FieldPeerRxId, FieldMajor, FieldMinor, FieldPatch>,
                             PacketComponentEnd>;
using PacketInitiateFail =
    PacketComponentContainer<std::tuple<FieldPsk, FieldMajor, FieldMinor, FieldPatch>, PacketComponentEnd>;
using PacketKeepalive = PacketComponentContainer<std::tuple<FieldPsk, FieldKeepaliveFlag>, PacketComponentEnd>;
using PacketInvalidPsk = PacketComponentContainer<std::tuple<FieldPsk>, PacketComponentEnd>;
using PacketInvalidChannel = PacketComponentContainer<std::tuple<FieldChannel>, PacketComponentEnd>;
using PacketInvalidAddress = PacketComponentContainer<std::tuple<FieldChannel>, PacketComponentEnd>;

using PacketControl =
    PacketComponentEnumMap<std::tuple<PacketComponentEnumMapEntry<MsgType::kInitiate, PacketInitiate>,
                                      PacketComponentEnumMapEntry<MsgType::kInitiateFail, PacketInitiateFail>,
                                      PacketComponentEnumMapEntry<MsgType::kKeepalive, PacketKeepalive>,
                                      PacketComponentEnumMapEntry<MsgType::kInvalidPsk, PacketInvalidPsk>,
                                      PacketComponentEnumMapEntry<MsgType::kInvalidChannel, PacketInvalidChannel>,
                                      PacketComponentEnumMapEntry<MsgType::kInvalidAddress, PacketInvalidAddress>>>;

using PacketUdpDynMux =
    PacketComponentEnumMap<std::tuple<PacketComponentEnumMapEntry<EnumChannel::kControlChannel, PacketControl>>,
                           PacketComponentEnd>;

using PacketUdpDynMuxPreParser = PacketComponentContainer<std::tuple<FieldChannel>, PacketComponentEnd>;

} // namespace gh::UdpDynMuxProto
