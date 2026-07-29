#include "ConnectionTracker.hpp"

#include <array>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/log/trivial.hpp>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>
#include <variant>

#include "Asio.hpp"
#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "Packet.hpp"
#include "PacketHeader.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"
#include "Utils/Endian.hpp"
#include "Utils/Overload.hpp"

namespace gh {

namespace {

auto UpdateField16(uint16_t& oldField, uint16_t newField) -> uint32_t {
  auto old = oldField;
  oldField = newField;
  return static_cast<uint16_t>(~old) + newField;
}

auto UpdateField32(uint32_t& oldField, uint32_t newField) -> uint32_t {
  auto oldData = std::bit_cast<std::array<uint16_t, 2>>(oldField);
  auto newData = std::bit_cast<std::array<uint16_t, 2>>(newField);
  oldField = newField;
  return static_cast<uint16_t>(~oldData[0]) + newData[0] + static_cast<uint16_t>(~oldData[1]) + newData[1];
}

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
auto UpdateField128(std::array<uint8_t, 16>& oldField, const std::array<uint8_t, 16>& newField) -> uint32_t {
  auto oldData = std::bit_cast<std::array<uint16_t, 8>>(oldField);
  auto newData = std::bit_cast<std::array<uint16_t, 8>>(newField);
  oldField = newField;
  uint32_t sum = 0;
  for (const auto [oldV, newV] : std::views::zip(oldData, newData)) {
    sum += static_cast<uint16_t>(~oldV) + newV;
  }
  return sum;
}

void FinalizeChecksum(uint16_t& checksum, uint32_t changes) {
  uint32_t total = static_cast<uint16_t>(~checksum) + changes;
  while ((total >> 16) != 0U) {
    total = (total & 0xFFFF) + (total >> 16);
  }
  checksum = static_cast<uint16_t>(~total);
}
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

} // namespace

template <typename Direction, typename EntryType>
void ConnectionTracker::UpdateEntryState(EntryType& entry,
                                         const typename EntryType::KeyType::State::ExtraKeyType& extra) {
  entry.State.template UpdateState<Direction>(extra);
}

ConnectionTracker::ConnectionTracker(boost::asio::any_io_executor executor) : _Executor(std::move(executor)) {}

auto ConnectionTracker::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> { co_return ErrorCode{}; }

auto ConnectionTracker::DoWork() -> Omni::Fiber::Coroutine<void> {
  boost::asio::steady_timer timer(_Executor);
  timer.expires_after(ConnectionState::ProneInterval);
  while (_State == State::kRunning && !_Service.value()._Stop.IsTriggered()) {
    auto [stop, timerFired] = co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(), [] -> void {}),
        Omni::Fiber::SelectPair(timer.async_wait(_Service.value()._Stop.AsioSlot()()),
                                Omni::Fiber::AsioApply([](auto err) -> auto { return err; })));

    if (stop) {
      break;
    }

    if (timerFired && !timerFired.value()) {
      auto now = std::chrono::steady_clock::now();
      assert(_OutputTable.size() == _InputTable.size());
      std::vector<ConnectionEntryPtr> expired;

      for (const auto& entryPtr : _OutputTable) {
        std::visit(
            [&](const auto& entry) -> void {
              if (entry.State.IsExpired(now)) {
                expired.push_back(entryPtr);
              }
            },
            *entryPtr);
      }

      for (const auto& entryPtr : expired) {
        _OutputTable.erase(entryPtr);
        _InputTable.erase(entryPtr);
      }

      timer.expires_after(ConnectionState::ProneInterval);
    }
  }
}

auto ConnectionTracker::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> {
  Clear();
  co_return ErrorCode{};
}

template <typename Direction> void ConnectionTracker::ApplySnat(Packet& packet, const ConnectionKey& key) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  reinterpret_cast<IPHeader*>(packet.Data().data())
      ->As(packet.Data(), false,
           Overload{
               [&](std::span<uint8_t> ip4span, IPv4Header* ip4) -> void {
                 ip4->Next(
                     ip4span, false,
                     Overload{
                         [&](std::span<uint8_t> /*tcpspan*/, TCPHeader* tcp) -> void {
                           assert(std::holds_alternative<Ip4TcpKey>(key));
                           const auto& nat = std::get<Ip4TcpKey>(key);
                           if constexpr (std::is_same_v<Direction, ConnectionDirectionOutput>) {
                             uint32_t ipChanges = UpdateField32(ip4->SrcIp, ArchEndian(nat.LocalAddress.to_uint()));
                             uint32_t portChanges = UpdateField16(tcp->SrcPort, ArchEndian(nat.LocalPort));
                             FinalizeChecksum(tcp->Checksum, ipChanges + portChanges);
                             FinalizeChecksum(ip4->Checksum, ipChanges);
                           } else {
                             uint32_t ipChanges = UpdateField32(ip4->DestIp, ArchEndian(nat.LocalAddress.to_uint()));
                             uint32_t portChanges = UpdateField16(tcp->DestPort, ArchEndian(nat.LocalPort));
                             FinalizeChecksum(tcp->Checksum, ipChanges + portChanges);
                             FinalizeChecksum(ip4->Checksum, ipChanges);
                           }
                         },
                         [&](std::span<uint8_t> /*udpspan*/, UDPHeader* udp) -> void {
                           assert(std::holds_alternative<Ip4UdpKey>(key));
                           const auto& nat = std::get<Ip4UdpKey>(key);
                           if constexpr (std::is_same_v<Direction, ConnectionDirectionOutput>) {
                             uint32_t ipChanges = UpdateField32(ip4->SrcIp, ArchEndian(nat.LocalAddress.to_uint()));
                             uint32_t portChanges = UpdateField16(udp->SrcPort, ArchEndian(nat.LocalPort));
                             FinalizeChecksum(udp->Checksum, ipChanges + portChanges);
                             FinalizeChecksum(ip4->Checksum, ipChanges);
                           } else {
                             uint32_t ipChanges = UpdateField32(ip4->DestIp, ArchEndian(nat.LocalAddress.to_uint()));
                             uint32_t portChanges = UpdateField16(udp->DestPort, ArchEndian(nat.LocalPort));
                             FinalizeChecksum(udp->Checksum, ipChanges + portChanges);
                             FinalizeChecksum(ip4->Checksum, ipChanges);
                           }
                         },
                         [&](std::span<uint8_t> /*icmpspan*/, ICMPv4Header* icmp) -> void {
                           assert(std::holds_alternative<IcmpKey>(key));
                           const auto& nat = std::get<IcmpKey>(key);
                           uint32_t ipChanges = 0;
                           if constexpr (std::is_same_v<Direction, ConnectionDirectionOutput>) {
                             ipChanges = UpdateField32(ip4->SrcIp, ArchEndian(nat.LocalAddress.to_uint()));
                           } else {
                             ipChanges = UpdateField32(ip4->DestIp, ArchEndian(nat.LocalAddress.to_uint()));
                           }
                           FinalizeChecksum(ip4->Checksum, ipChanges);
                           uint32_t idChanges = UpdateField16(icmp->Body.Echo.Id, ArchEndian(nat.Id));
                           FinalizeChecksum(icmp->Checksum, idChanges);
                         },
                         [&](std::span<uint8_t> /*span*/, std::string err) -> void {
                           BOOST_LOG_TRIVIAL(info) << "ConnectionTracker: ApplyNat error " << err;
                         },
                     });
               },
               [&](std::span<uint8_t> ip6span, IPv6Header* ip6) -> void {
                 ip6->Next( //
                     ip6span, false,
                     Overload{
                         [&](this auto& self, std::span<uint8_t> hopByHopSpan, IPv6HopByHopHeader* hopByHop) -> void {
                           return hopByHop->Next(hopByHopSpan, false, self);
                         },
                         [&](std::span<uint8_t> /*tcpspan*/, TCPHeader* tcp) -> void {
                           assert(std::holds_alternative<Ip6TcpKey>(key));
                           const auto& nat = std::get<Ip6TcpKey>(key);
                           if constexpr (std::is_same_v<Direction, ConnectionDirectionOutput>) {
                             uint32_t ipChanges = UpdateField128(ip6->SrcIp, nat.LocalAddress.to_bytes());
                             uint32_t portChanges = UpdateField16(tcp->SrcPort, ArchEndian(nat.LocalPort));
                             FinalizeChecksum(tcp->Checksum, ipChanges + portChanges);
                           } else {
                             uint32_t ipChanges = UpdateField128(ip6->DestIp, nat.LocalAddress.to_bytes());
                             uint32_t portChanges = UpdateField16(tcp->DestPort, ArchEndian(nat.LocalPort));
                             FinalizeChecksum(tcp->Checksum, ipChanges + portChanges);
                           }
                         },
                         [&](std::span<uint8_t> /*udpspan*/, UDPHeader* udp) -> void {
                           assert(std::holds_alternative<Ip6UdpKey>(key));
                           const auto& nat = std::get<Ip6UdpKey>(key);
                           if constexpr (std::is_same_v<Direction, ConnectionDirectionOutput>) {
                             uint32_t ipChanges = UpdateField128(ip6->SrcIp, nat.LocalAddress.to_bytes());
                             uint32_t portChanges = UpdateField16(udp->SrcPort, ArchEndian(nat.LocalPort));
                             FinalizeChecksum(udp->Checksum, ipChanges + portChanges);
                           } else {
                             uint32_t ipChanges = UpdateField128(ip6->DestIp, nat.LocalAddress.to_bytes());
                             uint32_t portChanges = UpdateField16(udp->DestPort, ArchEndian(nat.LocalPort));
                             FinalizeChecksum(udp->Checksum, ipChanges + portChanges);
                           }
                         },
                         [&](std::span<uint8_t> /*icmpspan*/, ICMPv6Header* icmpv6) -> void {
                           assert(std::holds_alternative<Icmp6Key>(key));
                           const auto& nat = std::get<Icmp6Key>(key);
                           uint32_t ipChanges = 0;
                           if constexpr (std::is_same_v<Direction, ConnectionDirectionOutput>) {
                             ipChanges = UpdateField128(ip6->SrcIp, nat.LocalAddress.to_bytes());
                           } else {
                             ipChanges = UpdateField128(ip6->DestIp, nat.LocalAddress.to_bytes());
                           }
                           uint32_t idChanges = UpdateField16(icmpv6->Body.Echo.Id, ArchEndian(nat.Id));
                           FinalizeChecksum(icmpv6->Checksum, ipChanges + idChanges);
                         },
                         [&](std::span<uint8_t> /*span*/, std::string err) -> void {
                           BOOST_LOG_TRIVIAL(info) << "ConnectionTracker: ApplyNat error " << err;
                         },
                     });
               },
               [&](std::span<uint8_t> /*span*/, std::string err) -> void {
                 BOOST_LOG_TRIVIAL(info) << "ConnectionTracker: ApplyNat error " << err;
               },
           });
}

template <typename ConnectionKeyType, typename Nat>
auto ConnectionTracker::BuildNatKey(const ConnectionKeyType& orig, const Nat& nat) -> std::optional<ConnectionKeyType> {
  assert((std::is_same_v<decltype(orig.LocalAddress), decltype(nat.LocalAddress)>));
  if constexpr (std::is_same_v<decltype(orig.LocalAddress), decltype(nat.LocalAddress)>) {
    auto tryBuildAndCheck = [&](std::optional<uint16_t> candidate) -> std::optional<ConnectionKeyType> {
      ConnectionKeyType result = orig;
      result.LocalAddress = nat.LocalAddress;
      if constexpr (requires { result.LocalPort; }) {
        result.LocalPort = candidate.value_or(orig.LocalPort);
      } else if constexpr (requires { result.Id; }) {
        result.Id = candidate.value_or(orig.Id);
      } else {
        static_assert(false);
        std::unreachable();
      }

      if (!_InputTable.contains(result)) {
        return result;
      }
      return std::nullopt;
    };

    auto res = tryBuildAndCheck(nat.LocalPort);
    if (res.has_value()) {
      return res;
    }

    if (nat.LocalPort.has_value()) {
      return std::nullopt;
    }

    for (uint16_t port = 49152; port <= 65535; ++port) {
      res = tryBuildAndCheck(port);
      if (res.has_value()) {
        return res;
      }
    }

    return std::nullopt;
  } else {
    assert(false);
    std::unreachable();
  }
}

template <typename Direction>
auto ConnectionTracker::LookupAndUpdate(Packet& packet, ConnectionTracker::Selector& selector) -> Result {
  auto now = std::chrono::steady_clock::now();
  return ParseConnectionKey<Direction>(
      packet.Data(), PacketType::kRealPacket, [&](auto&& parsedKey, PacketType type, auto keyExtra) -> Result {
        const ConnectionKey connKey = parsedKey;
        using ConnectionKeyType = std::decay_t<decltype(parsedKey)>;
        static_assert(std::is_same_v<decltype(keyExtra), typename ConnectionKeyType::State::ExtraKeyType>);

        auto [ThisDirectionTable, ThatDirectionTable] = [this] -> auto {
          if constexpr (std::is_same_v<Direction, ConnectionDirectionOutput>) {
            return std::make_tuple(std::ref(_OutputTable), std::ref(_InputTable));
          } else {
            return std::make_tuple(std::ref(_InputTable), std::ref(_OutputTable));
          }
        }();

        auto ApplySnatHelper = [](Packet& packet, const auto& entry) -> void {
          if constexpr (std::decay_t<decltype(entry)>::IsNat) {
            if constexpr (std::is_same_v<Direction, ConnectionDirectionOutput>) {
              ApplySnat<Direction>(packet, ConnectionKey(entry.NatKey));
            } else {
              ApplySnat<Direction>(packet, ConnectionKey(entry.Key));
            }
          } else {
            static_assert(std::decay_t<decltype(entry)>::IsNat == false);
          }
        };

        if (type == PacketType::kRealPacket) {
          auto iterator = ThisDirectionTable.find(connKey);
          if (iterator != ThisDirectionTable.end()) {
            auto mark = std::visit(
                [&](auto& entry) -> std::optional<std::shared_ptr<ConnectionMark>> {
                  if constexpr (std::is_same_v<typename std::decay_t<decltype(entry)>::KeyType, ConnectionKeyType>) {
                    if (entry.State.IsExpired(now)) {
                      auto entryPtr = *iterator;
                      ThisDirectionTable.erase(iterator);
                      ThatDirectionTable.erase(entryPtr);
                      return std::nullopt;
                    }

                    if (!entry.State.ConnectionEntryMark->Validate()) {
                      auto entryPtr = *iterator;
                      ThisDirectionTable.erase(iterator);
                      ThatDirectionTable.erase(entryPtr);
                      return std::nullopt;
                    }

                    entry.State.LastActive = now;
                    UpdateEntryState<Direction>(entry, keyExtra);
                    ApplySnatHelper(packet, entry);
                    return entry.State.ConnectionEntryMark;
                  } else {
                    assert(false);
                    std::unreachable();
                  }
                },
                **iterator);
            if (mark.has_value()) {
              return mark.value();
            }
          }

          auto action = selector.Select(connKey);
          ConnectionEntryPtr newEntry;
          auto natKey = std::visit( //
              Overload{
                  [&](Selector::Action::Snat4& snat) -> std::optional<ConnectionKeyType> {
                    return BuildNatKey(parsedKey, snat);
                  },
                  [&](Selector::Action::Snat6& snat) -> std::optional<ConnectionKeyType> {
                    return BuildNatKey(parsedKey, snat);
                  },
                  [&](std::monostate) -> std::optional<ConnectionKeyType> { return std::nullopt; },
              },
              action.Nat);
          if (natKey.has_value()) {
            newEntry = std::make_shared<TrackedConnectionEntry>(ConnectionNatEntry<ConnectionKeyType>{
                .Key = parsedKey,
                .NatKey = natKey.value(),
                .State = typename ConnectionKeyType::State{
                    std::in_place_type<Direction>, [&] -> std::shared_ptr<ConnectionMark> { return action.Mark; }, now,
                    keyExtra}});
          } else {
            newEntry = std::make_shared<TrackedConnectionEntry>(ConnectionEntry<ConnectionKeyType>{
                .Key = parsedKey,
                .State = typename ConnectionKeyType::State{
                    std::in_place_type<Direction>, [&] -> std::shared_ptr<ConnectionMark> { return action.Mark; }, now,
                    keyExtra}});
          }

          ThisDirectionTable.insert(newEntry);
          ThatDirectionTable.insert(newEntry);
          std::visit([&](auto& entry) -> void { ApplySnatHelper(packet, entry); }, *newEntry);
          return action.Mark;
        } else {
          if (auto iterator = ThisDirectionTable.find(connKey); iterator != ThisDirectionTable.end()) {
            return std::visit(
                [&](const auto& entry) -> Result {
                  if (entry.State.Validate(now)) {
                    // TODO: update ICMP error packet inner packet's IP address.
                    return entry.State.ConnectionEntryMark;
                  }
                  return std::unexpected(Error(AppMinorErrorCategory::kUnsupportedPacket));
                },
                **iterator);
          }
          return std::unexpected(Error(AppMinorErrorCategory::kUnsupportedPacket));
        }
      });
}

template auto ConnectionTracker::LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(
    Packet& packet, ConnectionTracker::Selector& selector) -> Result;

template auto ConnectionTracker::LookupAndUpdate<ConnectionTracker::ConnectionDirectionInput>(
    Packet& packet, ConnectionTracker::Selector& selector) -> Result;

void ConnectionTracker::Clear() {
  _OutputTable.clear();
  _InputTable.clear();
}

auto ConnectionTracker::GetConnections() const -> std::vector<TrackedEntry> {
  std::vector<TrackedEntry> connections;
  connections.reserve(_OutputTable.size());
  for (const auto& entryPtr : _OutputTable) {
    connections.push_back(TrackedEntry{
        .Key = GetOutputKey(*entryPtr),
        .Mark = std::visit(
            [](const auto& entry) -> std::string { return entry.State.ConnectionEntryMark->GetDescription(); },
            *entryPtr)});
  }
  return connections;
}

template <typename KeyDirection>
auto ConnectionTracker::ParseConnectionKey(std::span<uint8_t> packet, PacketType type, auto&& function) -> Result {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return reinterpret_cast<IPHeader*>(packet.data())
      ->As(packet, type != PacketType::kRealPacket,
           Overload{
               [&](std::span<uint8_t> ip4span, IPv4Header* ip4) -> Result {
                 auto srcAddr = boost::asio::ip::make_address_v4(ip4->GetSrcIp());
                 auto dstAddr = boost::asio::ip::make_address_v4(ip4->GetDestIp());
                 return ip4->Next(
                     ip4span, type != PacketType::kRealPacket,
                     Overload{
                         [&](std::span<uint8_t> /*tcpspan*/, TCPHeader* tcp) -> Result {
                           auto srcPort = tcp->GetSrcPort();
                           auto dstPort = tcp->GetDestPort();
                           TcpState::TcpExtraKey extra{.Flags = tcp->Flags,
                                                       .SequenceNumber = tcp->GetSeqNum(),
                                                       .AcknowledgementNumber = tcp->GetAckNum()};
                           if constexpr (std::is_same_v<KeyDirection, ConnectionDirectionOutput>) {
                             return function(Ip4TcpKey{.LocalAddress = srcAddr,
                                                       .RemoteAddress = dstAddr,
                                                       .LocalPort = srcPort,
                                                       .RemotePort = dstPort},
                                             type, extra);
                           } else {
                             return function(Ip4TcpKey{.LocalAddress = dstAddr,
                                                       .RemoteAddress = srcAddr,
                                                       .LocalPort = dstPort,
                                                       .RemotePort = srcPort},
                                             type, extra);
                           }
                         },
                         [&](std::span<uint8_t> /*udpspan*/, UDPHeader* udp) -> Result {
                           auto srcPort = udp->GetSrcPort();
                           auto dstPort = udp->GetDestPort();
                           if constexpr (std::is_same_v<KeyDirection, ConnectionDirectionOutput>) {
                             return function(Ip4UdpKey{.LocalAddress = srcAddr,
                                                       .RemoteAddress = dstAddr,
                                                       .LocalPort = srcPort,
                                                       .RemotePort = dstPort},
                                             type, Nothing{});
                           } else {
                             return function(Ip4UdpKey{.LocalAddress = dstAddr,
                                                       .RemoteAddress = srcAddr,
                                                       .LocalPort = dstPort,
                                                       .RemotePort = srcPort},
                                             type, Nothing{});
                           }
                         },
                         [&](std::span<uint8_t> icmpspan, ICMPv4Header* icmp) -> Result {
                           if (icmp->GetType() == IcmpType::EchoRequest || icmp->GetType() == IcmpType::EchoReply) {
                             uint16_t icmpId = icmp->GetEchoId();
                             if constexpr (std::is_same_v<KeyDirection, ConnectionDirectionOutput>) {
                               return function(IcmpKey{.LocalAddress = srcAddr, .RemoteAddress = dstAddr, .Id = icmpId},
                                               type, Nothing{});
                             } else {
                               return function(IcmpKey{.LocalAddress = dstAddr, .RemoteAddress = srcAddr, .Id = icmpId},
                                               type, Nothing{});
                             }
                           } else if (icmp->GetType() == IcmpType::DestinationUnreachable) {
                             return ParseConnectionKey<typename KeyDirection::OppositeDirection>(
                                 icmpspan.template subspan<sizeof(ICMPv4Header)>(), PacketType::kIcmpInnerPacket,
                                 std::forward<decltype(function)>(function));
                           }
                           return std::unexpected(Error(AppMinorErrorCategory::kUnsupportedPacket));
                         },
                         [&](std::span<uint8_t> /*span*/, std::string err) -> Result {
                           BOOST_LOG_TRIVIAL(info) << std::format("ConnectionTracker: {} -> {} {}", srcAddr.to_string(),
                                                                  dstAddr.to_string(), err);
                           return std::unexpected(Error(AppMinorErrorCategory::kUnsupportedPacket));
                         },
                     });
               },
               [&](std::span<uint8_t> ip6span, IPv6Header* ip6) -> Result {
                 auto srcAddr = boost::asio::ip::make_address_v6(ip6->SrcIp);
                 auto dstAddr = boost::asio::ip::make_address_v6(ip6->DestIp);
                 return ip6->Next(
                     ip6span, type != PacketType::kRealPacket,
                     Overload{
                         [&](this auto& self, std::span<uint8_t> hopByHopSpan, IPv6HopByHopHeader* hopByHop) -> Result {
                           return hopByHop->Next(hopByHopSpan, type != PacketType::kRealPacket, self);
                         },
                         [&](std::span<uint8_t> /*tcpspan*/, TCPHeader* tcp) -> Result {
                           auto srcPort = tcp->GetSrcPort();
                           auto dstPort = tcp->GetDestPort();
                           TcpState::TcpExtraKey extra{.Flags = tcp->Flags,
                                                       .SequenceNumber = tcp->GetSeqNum(),
                                                       .AcknowledgementNumber = tcp->GetAckNum()};
                           if constexpr (std::is_same_v<KeyDirection, ConnectionDirectionOutput>) {
                             return function(Ip6TcpKey{.LocalAddress = srcAddr,
                                                       .RemoteAddress = dstAddr,
                                                       .LocalPort = srcPort,
                                                       .RemotePort = dstPort},
                                             type, extra);
                           } else {
                             return function(Ip6TcpKey{.LocalAddress = dstAddr,
                                                       .RemoteAddress = srcAddr,
                                                       .LocalPort = dstPort,
                                                       .RemotePort = srcPort},
                                             type, extra);
                           }
                         },
                         [&](std::span<uint8_t> /*udpspan*/, UDPHeader* udp) -> Result {
                           auto srcPort = udp->GetSrcPort();
                           auto dstPort = udp->GetDestPort();
                           if constexpr (std::is_same_v<KeyDirection, ConnectionDirectionOutput>) {
                             return function(Ip6UdpKey{.LocalAddress = srcAddr,
                                                       .RemoteAddress = dstAddr,
                                                       .LocalPort = srcPort,
                                                       .RemotePort = dstPort},
                                             type, Nothing{});
                           } else {
                             return function(Ip6UdpKey{.LocalAddress = dstAddr,
                                                       .RemoteAddress = srcAddr,
                                                       .LocalPort = dstPort,
                                                       .RemotePort = srcPort},
                                             type, Nothing{});
                           }
                         },
                         [&](std::span<uint8_t> icmp6span, ICMPv6Header* icmp6) -> Result {
                           if (icmp6->GetType() == Icmp6Type::EchoRequest || icmp6->GetType() == Icmp6Type::EchoReply) {
                             uint16_t icmp6Id = icmp6->GetEchoId();
                             if constexpr (std::is_same_v<KeyDirection, ConnectionDirectionOutput>) {
                               return function(
                                   Icmp6Key{.LocalAddress = srcAddr, .RemoteAddress = dstAddr, .Id = icmp6Id}, type,
                                   Nothing{});
                             } else {
                               return function(
                                   Icmp6Key{.LocalAddress = dstAddr, .RemoteAddress = srcAddr, .Id = icmp6Id}, type,
                                   Nothing{});
                             }
                           } else if (icmp6->GetType() == Icmp6Type::DestinationUnreachable) {
                             return ParseConnectionKey<typename KeyDirection::OppositeDirection>(
                                 icmp6span.template subspan<sizeof(ICMPv6Header)>(), PacketType::kIcmpInnerPacket,
                                 std::forward<decltype(function)>(function));
                           }
                           return std::unexpected(Error(AppMinorErrorCategory::kUnsupportedPacket));
                         },
                         [&](std::span<uint8_t> /*span*/, std::string err) -> Result {
                           BOOST_LOG_TRIVIAL(info) << std::format("ConnectionTracker: {} -> {} {}", srcAddr.to_string(),
                                                                  dstAddr.to_string(), err);
                           return std::unexpected(Error(AppMinorErrorCategory::kUnsupportedPacket));
                         },
                     });
               },
               [&](std::span<uint8_t> /*span*/, std::string err) -> Result {
                 BOOST_LOG_TRIVIAL(info) << std::format("ConnectionTracker: {}", err);
                 return std::unexpected(Error(AppMinorErrorCategory::kUnsupportedPacket));
               }});
}

} // namespace gh
