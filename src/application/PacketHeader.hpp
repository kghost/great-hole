#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <type_traits>
#include <utility>

#include "Utils/Endian.hpp"

namespace gh {

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
// NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)

// Enforce strict 1-byte alignment across all compilers to prevent padding
#pragma pack(push, 1)

// ============================================================================
// ETHERNET TYPE ENUMERATIONS (IEEE 802.3 Standard)
// ============================================================================
enum class EtherType : uint16_t { IPv4 = 0x0800, ARP = 0x0806, IPv6 = 0x86DD };

// ============================================================================
// IP PROTOCOL / NEXT HEADER ENUMERATIONS
// ============================================================================
enum class IPProtocol : uint8_t {
  HopByHop = 0,
  ICMPv4 = 1,
  IGMP = 2,
  IPv4IP = 4,
  TCP = 6,
  UDP = 17,
  IPv6IP = 41,
  IPv6Route = 43,
  IPv6Frag = 44,
  GRE = 47,
  ESP = 50,
  AH = 51,
  ICMPv6 = 58,
  NoNext = 59,
  IPv6Opts = 60
};

// ============================================================================
// 0. ETHERNET II HEADER (14 Bytes Fixed)
// ============================================================================
struct EthernetHeader {
  std::array<uint8_t, 6> DestMac;
  std::array<uint8_t, 6> SrcMac;
  uint16_t Type;

  // Getters with automated Endian Conversion
  [[nodiscard]] auto GetEtherType() const noexcept -> EtherType { return static_cast<EtherType>(ArchEndian(Type)); }
};
static_assert(std::is_trivially_copyable_v<EthernetHeader>);
static_assert(sizeof(EthernetHeader) == 14);

// ============================================================================
// 1. IP HEADER
// ============================================================================
struct IPHeader {
  uint8_t VerIhl; // Combined Version (4 bits) + IHL (4 bits)

  [[nodiscard]] auto GetVersion() const noexcept -> uint8_t { return (VerIhl >> 4) & 0x0F; }
  [[nodiscard]] auto GetIhl() const noexcept -> uint8_t { return VerIhl & 0x0F; }

  template <typename Overload>
  auto As(std::span<const uint8_t> self, bool truncated, Overload&& overload) const -> decltype(auto);
};
static_assert(std::is_trivially_copyable_v<IPHeader>);

// ============================================================================
// 1. IPv4 HEADER
// ============================================================================
struct IPv4Header {
  uint8_t VerIhl;          // Combined Version (4 bits) + IHL (4 bits)
  uint8_t Tos;             // Type of Service
  uint16_t TotalLength;    // Total Length
  uint16_t Id;             // Identification
  uint16_t FragmentOffset; // Flags (3 bits) + Fragment Offset (13 bits)
  uint8_t Ttl;             // Time to Live
  uint8_t Protocol;        // Protocol (6 = TCP, 17 = UDP)
  uint16_t Checksum;       // Header Checksum
  uint32_t SrcIp;          // Source IP Address
  uint32_t DestIp;         // Destination IP Address

  // Raw Field Getters with automated Endian Conversion
  [[nodiscard]] auto GetTotalLength() const noexcept -> uint16_t { return ArchEndian(TotalLength); }
  [[nodiscard]] auto GetId() const noexcept -> uint16_t { return ArchEndian(Id); }
  [[nodiscard]] auto GetFlagsAndFragmentOffset() const noexcept -> uint16_t { return ArchEndian(FragmentOffset); }
  [[nodiscard]] auto GetChecksum() const noexcept -> uint16_t { return ArchEndian(Checksum); }
  [[nodiscard]] auto GetSrcIp() const noexcept -> uint32_t { return ArchEndian(SrcIp); }
  [[nodiscard]] auto GetDestIp() const noexcept -> uint32_t { return ArchEndian(DestIp); }
  [[nodiscard]] auto GetProtocol() const noexcept -> IPProtocol { return static_cast<IPProtocol>(Protocol); }

  // Sub-byte Field Getters (safe against bit-ordering rules)
  [[nodiscard]] auto GetVersion() const noexcept -> uint8_t { return (VerIhl >> 4) & 0x0F; }
  [[nodiscard]] auto GetIhl() const noexcept -> uint8_t { return VerIhl & 0x0F; }
  [[nodiscard]] auto GetFlags() const noexcept -> uint8_t { return (GetFlagsAndFragmentOffset() & 0xE000) >> 13; }
  [[nodiscard]] auto GetFragmentOffset() const noexcept -> uint16_t { return GetFlagsAndFragmentOffset() & 0x1FFF; }

  auto As(std::span<const uint8_t> self, bool truncated, auto&& overload) const -> decltype(auto) {
    auto valid = self.size() >= static_cast<size_t>(GetIhl()) * 4 && (truncated || self.size() == GetTotalLength());
    return valid ? overload(self, this)
                 : overload(self, std::format("Invalid IP Header: size = {}, ihl = {}, total_length = {}", self.size(),
                                              GetIhl(), GetTotalLength()));
  }

  auto Next(std::span<const uint8_t> self, bool truncated, auto&& overload) const -> decltype(auto);
};
static_assert(std::is_trivially_copyable_v<IPv4Header>);
static_assert(sizeof(IPv4Header) == 20);

// ============================================================================
// 2. IPv6 HEADER (40 Bytes Fixed)
// ============================================================================
struct IPv6Header {
  uint32_t VTcFl;                 // Version (4b), Traffic Class (8b), Flow Label (20b)
  uint16_t PayloadLength;         // Payload Length
  uint8_t NextHeader;             // Next Header protocol type
  uint8_t HopLimit;               // Hop Limit (TTL replacement)
  std::array<uint8_t, 16> SrcIp;  // 128-bit Source Address (Byte arrays are inherently endian-neutral)
  std::array<uint8_t, 16> DestIp; // 128-bit Destination Address

  // Raw Field Getters with automated Endian Conversion
  [[nodiscard]] auto GetVTcFl() const noexcept -> uint32_t { return ArchEndian(VTcFl); }
  [[nodiscard]] auto GetPayloadLength() const noexcept -> uint16_t { return ArchEndian(PayloadLength); }
  [[nodiscard]] auto GetNextHeader() const noexcept -> IPProtocol { return static_cast<IPProtocol>(NextHeader); }

  // Portable Version extraction: Convert first, then parse.
  // This makes it completely platform-agnostic and immune to host endianness.
  [[nodiscard]] auto GetVersion() const noexcept -> uint8_t { return (GetVTcFl() >> 28) & 0x0F; }
  [[nodiscard]] auto GetTrafficClass() const noexcept -> uint8_t { return (GetVTcFl() >> 20) & 0xFF; }
  [[nodiscard]] auto GetFlowLabel() const noexcept -> uint32_t { return GetVTcFl() & 0x000FFFFF; }

  auto As(std::span<const uint8_t> self, bool truncated, auto&& overload) const -> decltype(auto) {
    auto valid =
        self.size() >= sizeof(IPv6Header) && (truncated || self.size() == sizeof(IPv6Header) + GetPayloadLength());
    return valid ? overload(self, this)
                 : overload(self, std::format("Invalid IPv6 Header: size = {}, payload_length = {}", self.size(),
                                              GetPayloadLength()));
  }

  static auto Next(IPProtocol protocol, std::span<const uint8_t> next, bool truncated, auto&& overload)
      -> decltype(auto);
  auto Next(std::span<const uint8_t> self, bool truncated, auto&& overload) const -> decltype(auto) {
    return Next(GetNextHeader(), self.template subspan<sizeof(IPv6Header)>(), truncated,
                std::forward<decltype(overload)>(overload));
  }
};
static_assert(std::is_trivially_copyable_v<IPv6Header>);
static_assert(sizeof(IPv6Header) == 40);

// ============================================================================
// 3. TCP HEADER (20 Bytes Minimum)
// ============================================================================
struct TCPHeader {
  uint16_t SrcPort;              // Source Port
  uint16_t DestPort;             // Destination Port
  uint32_t SeqNum;               // Sequence Number
  uint32_t AckNum;               // Acknowledgment Number
  uint8_t DataOffsetAndReserved; // Data Offset (4 bits) + Reserved (4 bits)
  uint8_t Flags;                 // TCP Flags (CWR, ECE, URG, ACK, PSH, RST, SYN, FIN)
  uint16_t WindowSize;           // Window Size
  uint16_t Checksum;             // Checksum
  uint16_t UrgPtr;               // Urgent Pointer

  // Raw Field Getters with automated Endian Conversion
  [[nodiscard]] auto GetSrcPort() const noexcept -> uint16_t { return ArchEndian(SrcPort); }
  [[nodiscard]] auto GetDestPort() const noexcept -> uint16_t { return ArchEndian(DestPort); }
  [[nodiscard]] auto GetSeqNum() const noexcept -> uint32_t { return ArchEndian(SeqNum); }
  [[nodiscard]] auto GetAckNum() const noexcept -> uint32_t { return ArchEndian(AckNum); }
  [[nodiscard]] auto GetWindowSize() const noexcept -> uint16_t { return ArchEndian(WindowSize); }
  [[nodiscard]] auto GetChecksum() const noexcept -> uint16_t { return ArchEndian(Checksum); }
  [[nodiscard]] auto GetUrgPtr() const noexcept -> uint16_t { return ArchEndian(UrgPtr); }

  // Sub-byte / Flag Field Getters
  [[nodiscard]] auto GetDataOffset() const noexcept -> uint8_t { return (DataOffsetAndReserved >> 4) & 0x0F; }
  [[nodiscard]] auto IsFin() const noexcept -> bool { return (Flags & 0x01) != 0; }
  [[nodiscard]] auto IsSyn() const noexcept -> bool { return (Flags & 0x02) != 0; }
  [[nodiscard]] auto IsRst() const noexcept -> bool { return (Flags & 0x04) != 0; }
  [[nodiscard]] auto IsPsh() const noexcept -> bool { return (Flags & 0x08) != 0; }
  [[nodiscard]] auto IsAck() const noexcept -> bool { return (Flags & 0x10) != 0; }
  [[nodiscard]] auto IsUrg() const noexcept -> bool { return (Flags & 0x20) != 0; }
  [[nodiscard]] auto IsEce() const noexcept -> bool { return (Flags & 0x40) != 0; }
  [[nodiscard]] auto IsCwr() const noexcept -> bool { return (Flags & 0x80) != 0; }

  auto As(std::span<const uint8_t> self, bool /*truncated*/, auto&& overload) const -> decltype(auto) {
    auto valid = self.size() >= static_cast<size_t>(GetDataOffset()) * 4;
    return valid ? overload(self, this)
                 : overload(self, std::format("Invalid TCP Header: size = {}, data_offset = {}", self.size(),
                                              GetDataOffset()));
  }
};
static_assert(std::is_trivially_copyable_v<TCPHeader>);
static_assert(sizeof(TCPHeader) == 20);

// ============================================================================
// 4. UDP HEADER (8 Bytes Fixed)
// ============================================================================
struct UDPHeader {
  uint16_t SrcPort;
  uint16_t DestPort;
  uint16_t Length;
  uint16_t Checksum;

  // Raw Field Getters with automated Endian Conversion
  [[nodiscard]] auto GetSrcPort() const noexcept -> uint16_t { return ArchEndian(SrcPort); }
  [[nodiscard]] auto GetDestPort() const noexcept -> uint16_t { return ArchEndian(DestPort); }
  [[nodiscard]] auto GetLength() const noexcept -> uint16_t { return ArchEndian(Length); }
  [[nodiscard]] auto GetChecksum() const noexcept -> uint16_t { return ArchEndian(Checksum); }

  auto As(std::span<const uint8_t> self, bool truncated, auto&& overload) const -> decltype(auto) {
    auto valid = self.size() >= sizeof(UDPHeader) && (truncated || self.size() == GetLength());
    return valid ? overload(self, this)
                 : overload(self, std::format("Invalid UDP Header: size = {}, length = {}", self.size(), GetLength()));
  }
};
static_assert(std::is_trivially_copyable_v<UDPHeader>);
static_assert(sizeof(UDPHeader) == 8);

// ============================================================================
// 5. ICMP (v4) HEADER
// ============================================================================
enum struct IcmpType : uint8_t {
  EchoReply = 0,
  DestinationUnreachable = 3,
  SourceQuench = 4,
  Redirect = 5,
  EchoRequest = 8,
  TimeExceeded = 11,
  ParameterProblem = 12,
  TimestampRequest = 13,
  TimestampReply = 14
};

// Codes for Type 3: Destination Unreachable
enum struct IcmpCodeDestinationUnreachable : uint8_t {
  NetUnreachable = 0,
  HostUnreachable = 1,
  ProtocolUnreachable = 2,
  PortUnreachable = 3,
  FragmentationNeeded = 4, // DF bit set
  SourceRouteFailed = 5,
  NetUnknown = 6,
  HostUnknown = 7,
  SourceHostIsolated = 8,
  NetAdminProhibited = 9,
  HostAdminProhibited = 10,
  NetUnreachableForToS = 11,
  HostUnreachableForToS = 12,
  CommAdminProhibited = 13, // Firewall filter
  HostPrecedenceViolation = 14,
  PrecedenceCutoffInEffect = 15
};

// Codes for Type 5: Redirect
enum struct IcmpCodeRedirect : uint8_t { RedirectNet = 0, RedirectHost = 1, RedirectToSNet = 2, RedirectToSHost = 3 };

// Codes for Type 11: Time Exceeded
enum struct IcmpCodeTimeExceeded : uint8_t {
  TTLExceeded = 0, // Time-to-Live expired in transit
  ReassemblyTimeout = 1
};

// Codes for Type 12: Parameter Problem
enum struct IcmpCodeParameterProblem : uint8_t { PointerIndicatesError = 0, MissingRequiredOption = 1, BadLength = 2 };

struct ICMPv4Header {
  uint8_t Type;
  uint8_t Code;
  uint16_t Checksum;

  union {
    struct {
      uint16_t Id;
      uint16_t Sequence;
    } Echo;
    uint32_t GatewayIp;
    uint32_t Unused;
  } Body;

  // Raw Field Getters with automated Endian Conversion
  union IcmpCode {
    IcmpCodeDestinationUnreachable DestinationUnreachable;
    IcmpCodeRedirect Redirect;
    IcmpCodeTimeExceeded TimeExceeded;
    IcmpCodeParameterProblem ParameterProblem;
    uint8_t Raw;
  };

  [[nodiscard]] auto GetType() const noexcept -> IcmpType { return static_cast<IcmpType>(Type); }
  [[nodiscard]] auto GetCode() const noexcept -> IcmpCode { return IcmpCode{.Raw = Code}; }
  [[nodiscard]] auto GetChecksum() const noexcept -> uint16_t { return ArchEndian(Checksum); }
  [[nodiscard]] auto GetEchoId() const noexcept -> uint16_t { return ArchEndian(Body.Echo.Id); }
  [[nodiscard]] auto GetEchoSequence() const noexcept -> uint16_t { return ArchEndian(Body.Echo.Sequence); }
  [[nodiscard]] auto GetGatewayIp() const noexcept -> uint32_t { return ArchEndian(Body.GatewayIp); }

  auto As(std::span<const uint8_t> self, bool /*truncated*/, auto&& overload) const -> decltype(auto) {
    auto valid = self.size() >= sizeof(ICMPv4Header);
    return valid ? overload(self, this)
                 : overload(self, std::format("Invalid ICMPv4 Header: size = {}, expected = {}", self.size(),
                                              sizeof(ICMPv4Header)));
  }
};
static_assert(std::is_trivially_copyable_v<ICMPv4Header>);
static_assert(sizeof(ICMPv4Header) == 8);

// ============================================================================
// 6. ICMPv6 HEADER
// ============================================================================
enum struct Icmp6Type : uint8_t {
  // Error Messages (0-127)
  DestinationUnreachable = 1,
  PacketTooBig = 2,
  TimeExceeded = 3,
  ParameterProblem = 4,

  // Informational Messages (128-255)
  EchoRequest = 128,
  EchoReply = 129,
  MulticastQuery = 130,
  MulticastReport = 131,
  MulticastDone = 132,
  RouterSolicitation = 133,
  RouterAdvertisement = 134,
  NeighborSolicitation = 135,
  NeighborAdvertisement = 136,
  Redirect = 137
};

// Codes for Type 1: Destination Unreachable
enum struct Icmp6CodeDestinationUnreachable : uint8_t {
  NoRoute = 0,
  AdminProhibited = 1,
  BeyondScope = 2,
  AddressUnreachable = 3,
  PortUnreachable = 4,
  PolicyFailed = 5,
  RejectRoute = 6,
  SourceRoutingHeaderErr = 7
};

// Codes for Type 2: Packet Too Big
enum struct Icmp6CodePacketTooBig : uint8_t { Standard = 0 };

// Codes for Type 3: Time Exceeded
enum struct Icmp6CodeTimeExceeded : uint8_t { HopLimitExceeded = 0, ReassemblyTimeout = 1 };

// Codes for Type 4: Parameter Problem
enum struct Icmp6CodeParameterProblem : uint8_t {
  ErroneousHeaderField = 0,
  UnrecognizedNextHeader = 1,
  UnrecognizedOption = 2
};

// Default code value for all Informational Messages
enum struct Icmp6CodeInformational : uint8_t { Standard = 0 };

struct ICMPv6Header {
  uint8_t Type;
  uint8_t Code;
  uint16_t Checksum;

  union {
    struct {
      uint16_t Id;
      uint16_t Sequence;
    } Echo;
    uint32_t Mtu;
    uint32_t Pointer;
    uint32_t Reserved;
  } Body;

  union Icmp6Code {
    Icmp6CodeDestinationUnreachable DestinationUnreachable;
    Icmp6CodePacketTooBig PacketTooBig;
    Icmp6CodeTimeExceeded TimeExceeded;
    Icmp6CodeParameterProblem ParameterProblem;
    Icmp6CodeInformational Informational;
    uint8_t Raw;
  };

  // Raw Field Getters with automated Endian Conversion
  [[nodiscard]] auto GetType() const noexcept -> Icmp6Type { return static_cast<Icmp6Type>(Type); }
  [[nodiscard]] auto GetCode() const noexcept -> Icmp6Code { return Icmp6Code{.Raw = Code}; }
  [[nodiscard]] auto GetChecksum() const noexcept -> uint16_t { return ArchEndian(Checksum); }
  [[nodiscard]] auto GetEchoId() const noexcept -> uint16_t { return ArchEndian(Body.Echo.Id); }
  [[nodiscard]] auto GetEchoSequence() const noexcept -> uint16_t { return ArchEndian(Body.Echo.Sequence); }
  [[nodiscard]] auto GetMtu() const noexcept -> uint32_t { return ArchEndian(Body.Mtu); }
  [[nodiscard]] auto GetPointer() const noexcept -> uint32_t { return ArchEndian(Body.Pointer); }

  auto As(std::span<const uint8_t> self, bool /*truncated*/, auto&& overload) const -> decltype(auto) {
    auto valid = self.size() >= sizeof(ICMPv6Header);
    return valid ? overload(self, this)
                 : overload(self, std::format("Invalid ICMPv6 Header: size = {}, expected = {}", self.size(),
                                              sizeof(ICMPv6Header)));
  }
};
static_assert(std::is_trivially_copyable_v<ICMPv6Header>);
static_assert(sizeof(ICMPv6Header) == 8);

// ============================================================================
// 7. IPv6 HOP BY HOP HEADER
// ============================================================================

struct IPv6HopByHopHeader {
  uint8_t NextHeader;
  uint8_t HeaderExtLen;
  std::array<uint8_t, 6> Reserved;

  // Raw Field Getters with automated Endian Conversion
  [[nodiscard]] auto GetNextHeader() const noexcept -> IPProtocol { return static_cast<IPProtocol>(NextHeader); }
  [[nodiscard]] auto GetHeaderExtLen() const noexcept -> uint8_t { return HeaderExtLen; }
  [[nodiscard]] auto GetReserved() const noexcept -> std::span<const uint8_t, 6> { return Reserved; }

  auto As(std::span<const uint8_t> self, bool /*truncated*/, auto&& overload) const -> decltype(auto) {
    auto valid = self.size() >= sizeof(IPv6HopByHopHeader) + (static_cast<size_t>(GetHeaderExtLen()) * 8);
    return valid ? overload(self, this)
                 : overload(self, std::format("Invalid IPv6 HopByHop Header: size = {}, header_ext_len = {}",
                                              self.size(), GetHeaderExtLen()));
  }

  auto Next(std::span<const uint8_t> self, bool truncated, auto&& overload) const -> decltype(auto) {
    return IPv6Header::Next(GetNextHeader(),
                            self.subspan(sizeof(IPv6HopByHopHeader) + (static_cast<size_t>(GetHeaderExtLen()) * 8)),
                            truncated, std::forward<decltype(overload)>(overload));
  }
};
static_assert(std::is_trivially_copyable_v<IPv6HopByHopHeader>);
static_assert(sizeof(IPv6HopByHopHeader) == 8);

// ============================================================================
// IPHeader::As Implementation
// ============================================================================
auto IPHeader::As(std::span<const uint8_t> self, bool truncated, auto&& overload) const -> decltype(auto) {
  if (GetVersion() == 4 && self.size() >= sizeof(IPv4Header)) {
    return reinterpret_cast<const IPv4Header*>(this)->As(self, truncated, overload);
  } else if (GetVersion() == 6 && self.size() >= sizeof(IPv6Header)) {
    return reinterpret_cast<const IPv6Header*>(this)->As(self, truncated, overload);
  } else {
    return overload(self, std::format("Unknown IP version: {}", GetVersion()));
  }
}

// ============================================================================
// IPv4Header::Next Implementation
// ============================================================================
auto IPv4Header::Next(std::span<const uint8_t> self, bool truncated, auto&& overload) const -> decltype(auto) {
  auto next = self.subspan(static_cast<size_t>(GetIhl()) * 4);
  if (GetProtocol() == IPProtocol::TCP && next.size() >= sizeof(TCPHeader)) {
    return reinterpret_cast<const TCPHeader*>(next.data())->As(next, truncated, overload);
  } else if (GetProtocol() == IPProtocol::UDP && next.size() >= sizeof(UDPHeader)) {
    return reinterpret_cast<const UDPHeader*>(next.data())->As(next, truncated, overload);
  } else if (GetProtocol() == IPProtocol::ICMPv4 && next.size() >= sizeof(ICMPv4Header)) {
    return reinterpret_cast<const ICMPv4Header*>(next.data())->As(next, truncated, overload);
  } else {
    return overload(next, std::format("Unknown IPv4 payload type: {}", std::to_underlying(GetProtocol())));
  }
}

// ============================================================================
// IPv6Header::Next Implementation
// ============================================================================
auto IPv6Header::Next(IPProtocol protocol, std::span<const uint8_t> next, bool truncated, auto&& overload)
    -> decltype(auto) {
  switch (protocol) {
  case IPProtocol::TCP:
    if (next.size() >= sizeof(TCPHeader)) {
      return reinterpret_cast<const TCPHeader*>(next.data())->As(next, truncated, overload);
    }
    break;
  case IPProtocol::UDP:
    if (next.size() >= sizeof(UDPHeader)) {
      return reinterpret_cast<const UDPHeader*>(next.data())->As(next, truncated, overload);
    }
    break;
  case IPProtocol::ICMPv6:
    if (next.size() >= sizeof(ICMPv6Header)) {
      return reinterpret_cast<const ICMPv6Header*>(next.data())->As(next, truncated, overload);
    }
    break;
  case IPProtocol::HopByHop:
    if (next.size() >= sizeof(IPv6HopByHopHeader)) {
      return reinterpret_cast<const IPv6HopByHopHeader*>(next.data())->As(next, truncated, overload);
    }
    break;
  case IPProtocol::IPv6Opts:
  case IPProtocol::IPv6Frag:
  // Add other ICMPv6 message types or extension headers here if needed
  default:
    break;
  }
  return overload(next, std::format("Unknown IPv6 payload type: {}", std::to_underlying(protocol)));
}

#pragma pack(pop)

// NOLINTEND(cppcoreguidelines-pro-type-union-access)
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)

} // namespace gh
