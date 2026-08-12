#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ErrorCode.hpp"

namespace gh::dns {

// Standard DNS Record Types
enum class DnsType : uint16_t {
  A = 1,
  NS = 2,
  CNAME = 5,
  SOA = 6,
  PTR = 12,
  MX = 15,
  TXT = 16,
  AAAA = 28,
  SRV = 33,
  ANY = 255
};

// Standard DNS Response Codes (RCODE)
enum class DnsRCode : uint8_t { NoError = 0, FormErr = 1, ServFail = 2, NXDomain = 3, NotImp = 4, Refused = 5 };

constexpr uint16_t kDnsFlagQrMask = 0x8000;
constexpr uint16_t kDnsFlagRCodeMask = 0x000F;
constexpr uint16_t kDnsFlagRCodeClearMask = 0xFFF0;
constexpr uint32_t kDefaultDnsRecordTtl = 300;

struct DnsHeader {
  uint16_t Id{0};
  uint16_t Flags{0};
  uint16_t QdCount{0};
  uint16_t AnCount{0};
  uint16_t NsCount{0};
  uint16_t ArCount{0};

  [[nodiscard]] auto IsQuery() const -> bool { return (Flags & kDnsFlagQrMask) == 0; }
  [[nodiscard]] auto IsResponse() const -> bool { return (Flags & kDnsFlagQrMask) != 0; }
  void SetResponse(bool isResponse) {
    if (isResponse) {
      Flags |= kDnsFlagQrMask;
    } else {
      Flags &= ~kDnsFlagQrMask;
    }
  }

  [[nodiscard]] auto GetRCode() const -> DnsRCode { return static_cast<DnsRCode>(Flags & kDnsFlagRCodeMask); }
  void SetRCode(DnsRCode rcode) {
    Flags = (Flags & kDnsFlagRCodeClearMask) | (static_cast<uint16_t>(rcode) & kDnsFlagRCodeMask);
  }
};

struct DnsQuestion {
  std::string QName;
  uint16_t QType{std::to_underlying(DnsType::A)};
  uint16_t QClass{1}; // IN
};

struct DnsResourceRecord {
  std::string Name;
  uint16_t Type{std::to_underlying(DnsType::A)};
  uint16_t RClass{1}; // IN
  uint32_t Ttl{kDefaultDnsRecordTtl};
  std::vector<uint8_t> RData;
};

class DnsPacket {
public:
  DnsHeader Header;
  std::vector<DnsQuestion> Questions;
  std::vector<DnsResourceRecord> Answers;
  std::vector<DnsResourceRecord> Authorities;
  std::vector<DnsResourceRecord> Additionals;

  static auto Parse(std::span<const uint8_t> buffer) -> std::expected<DnsPacket, ErrorCode>;
  [[nodiscard]] auto Serialize() const -> std::vector<uint8_t>;

  [[nodiscard]] auto GetPrimaryQName() const -> std::string {
    if (!Questions.empty()) {
      return Questions[0].QName;
    }
    return "";
  }

  static auto MakeErrorResponse(uint16_t queryId, DnsRCode rcode) -> DnsPacket;
};

} // namespace gh::dns
