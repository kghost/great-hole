#include "DnsPacket.hpp"

#include <cstring>

namespace gh::dns {

namespace {

constexpr size_t kDnsHeaderSize = 12;
constexpr size_t kMaxPointerDepth = 16;

auto ReadUint16(std::span<const uint8_t> data, size_t offset) -> uint16_t {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
}

void WriteUint16(std::vector<uint8_t>& buf, uint16_t value) {
  buf.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>(value & 0xFF));
}

void WriteUint32(std::vector<uint8_t>& buf, uint32_t value) {
  buf.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
  buf.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  buf.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>(value & 0xFF));
}

auto ParseDomainName(std::span<const uint8_t> buffer, size_t& offset) -> std::expected<std::string, ErrorCode> {
  std::string name;
  size_t current = offset;
  size_t nextOffset = 0;
  bool jumped = false;
  size_t jumps = 0;

  while (current < buffer.size()) {
    uint8_t len = buffer[current];
    if (len == 0) {
      current++;
      if (!jumped) {
        nextOffset = current;
      }
      break;
    }

    // Check pointer (top 2 bits set)
    if ((len & 0xC0) == 0xC0) {
      if (current + 1 >= buffer.size()) {
        return std::unexpected(SysError(EINVAL));
      }
      if (!jumped) {
        nextOffset = current + 2;
        jumped = true;
      }
      size_t ptr = static_cast<size_t>(((len & 0x3F) << 8) | buffer[current + 1]);
      if (ptr >= buffer.size()) {
        return std::unexpected(SysError(EINVAL));
      }
      current = ptr;
      jumps++;
      if (jumps > kMaxPointerDepth) {
        return std::unexpected(SysError(ELOOP));
      }
      continue;
    }

    // Ordinary label
    current++;
    if (current + len > buffer.size()) {
      return std::unexpected(SysError(EINVAL));
    }

    if (!name.empty()) {
      name.push_back('.');
    }
    name.append(reinterpret_cast<const char*>(buffer.data() + current), len);
    current += len;
  }

  offset = jumped ? nextOffset : current;
  return name;
}

void EncodeDomainName(std::vector<uint8_t>& buf, const std::string& domain) {
  if (domain.empty() || domain == ".") {
    buf.push_back(0);
    return;
  }

  size_t start = 0;
  while (start < domain.size()) {
    size_t dot = domain.find('.', start);
    if (dot == std::string::npos) {
      dot = domain.size();
    }
    size_t len = dot - start;
    if (len > 0 && len <= 63) {
      buf.push_back(static_cast<uint8_t>(len));
      buf.insert(buf.end(), domain.begin() + static_cast<ptrdiff_t>(start),
                 domain.begin() + static_cast<ptrdiff_t>(dot));
    }
    start = dot + 1;
  }
  buf.push_back(0);
}

} // namespace

auto DnsPacket::Parse(std::span<const uint8_t> buffer) -> std::expected<DnsPacket, ErrorCode> {
  if (buffer.size() < kDnsHeaderSize) {
    return std::unexpected(SysError(EINVAL));
  }

  DnsPacket packet;
  packet.Header.Id = ReadUint16(buffer, 0);
  packet.Header.Flags = ReadUint16(buffer, 2);
  packet.Header.QdCount = ReadUint16(buffer, 4);
  packet.Header.AnCount = ReadUint16(buffer, 6);
  packet.Header.NsCount = ReadUint16(buffer, 8);
  packet.Header.ArCount = ReadUint16(buffer, 10);

  size_t offset = kDnsHeaderSize;

  // Parse Questions
  for (uint16_t i = 0; i < packet.Header.QdCount; ++i) {
    auto qnameRes = ParseDomainName(buffer, offset);
    if (!qnameRes) {
      return std::unexpected(qnameRes.error());
    }
    if (offset + 4 > buffer.size()) {
      return std::unexpected(SysError(EINVAL));
    }
    DnsQuestion q;
    q.QName = std::move(*qnameRes);
    q.QType = ReadUint16(buffer, offset);
    q.QClass = ReadUint16(buffer, offset + 2);
    offset += 4;
    packet.Questions.push_back(std::move(q));
  }

  auto parseRRs = [&](uint16_t count, std::vector<DnsResourceRecord>& section) -> std::expected<void, ErrorCode> {
    for (uint16_t i = 0; i < count; ++i) {
      auto nameRes = ParseDomainName(buffer, offset);
      if (!nameRes) {
        return std::unexpected(nameRes.error());
      }
      if (offset + 10 > buffer.size()) {
        return std::unexpected(SysError(EINVAL));
      }
      DnsResourceRecord rr;
      rr.Name = std::move(*nameRes);
      rr.Type = ReadUint16(buffer, offset);
      rr.RClass = ReadUint16(buffer, offset + 2);
      rr.Ttl = (static_cast<uint32_t>(buffer[offset + 4]) << 24) | (static_cast<uint32_t>(buffer[offset + 5]) << 16) |
               (static_cast<uint32_t>(buffer[offset + 6]) << 8) | static_cast<uint32_t>(buffer[offset + 7]);
      uint16_t rdlength = ReadUint16(buffer, offset + 8);
      offset += 10;
      if (offset + rdlength > buffer.size()) {
        return std::unexpected(SysError(EINVAL));
      }
      rr.RData.assign(buffer.begin() + static_cast<ptrdiff_t>(offset),
                      buffer.begin() + static_cast<ptrdiff_t>(offset + rdlength));
      offset += rdlength;
      section.push_back(std::move(rr));
    }
    return {};
  };

  if (auto err = parseRRs(packet.Header.AnCount, packet.Answers); !err) {
    return std::unexpected(err.error());
  }
  if (auto err = parseRRs(packet.Header.NsCount, packet.Authorities); !err) {
    return std::unexpected(err.error());
  }
  if (auto err = parseRRs(packet.Header.ArCount, packet.Additionals); !err) {
    return std::unexpected(err.error());
  }

  return packet;
}

auto DnsPacket::Serialize() const -> std::vector<uint8_t> {
  std::vector<uint8_t> buf;
  buf.reserve(512);

  WriteUint16(buf, Header.Id);
  WriteUint16(buf, Header.Flags);
  WriteUint16(buf, static_cast<uint16_t>(Questions.size()));
  WriteUint16(buf, static_cast<uint16_t>(Answers.size()));
  WriteUint16(buf, static_cast<uint16_t>(Authorities.size()));
  WriteUint16(buf, static_cast<uint16_t>(Additionals.size()));

  for (const auto& q : Questions) {
    EncodeDomainName(buf, q.QName);
    WriteUint16(buf, q.QType);
    WriteUint16(buf, q.QClass);
  }

  auto writeRRs = [&](const std::vector<DnsResourceRecord>& section) {
    for (const auto& rr : section) {
      EncodeDomainName(buf, rr.Name);
      WriteUint16(buf, rr.Type);
      WriteUint16(buf, rr.RClass);
      WriteUint32(buf, rr.Ttl);
      WriteUint16(buf, static_cast<uint16_t>(rr.RData.size()));
      buf.insert(buf.end(), rr.RData.begin(), rr.RData.end());
    }
  };

  writeRRs(Answers);
  writeRRs(Authorities);
  writeRRs(Additionals);

  return buf;
}

auto DnsPacket::MakeErrorResponse(uint16_t queryId, DnsRCode rcode) -> DnsPacket {
  DnsPacket packet;
  packet.Header.Id = queryId;
  packet.Header.SetResponse(true);
  packet.Header.SetRCode(rcode);
  return packet;
}

} // namespace gh::dns
