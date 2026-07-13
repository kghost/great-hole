#include "ConnectionTracker.hpp"

#include <array>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

#include "Asio.hpp"
#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "Packet.hpp"
#include "PacketHeader.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"
#include "Utils/Overload.hpp"

namespace gh {

ConnectionTracker::ConnectionTracker(boost::asio::any_io_executor executor) : _Executor(std::move(executor)) {}

auto ConnectionTracker::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> { co_return ErrorCode{}; }

auto ConnectionTracker::DoWork() -> Omni::Fiber::Coroutine<void> {
  boost::asio::steady_timer timer(_Executor);
  timer.expires_after(ConnectionEntry::ProneInterval);
  while (_State == State::kRunning && !_Service.value()._Stop.IsTriggered()) {
    auto [stop, timerFired] = co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(), [] -> void {}),
        Omni::Fiber::SelectPair(timer.async_wait(_Service.value()._Stop.AsioSlot()()),
                                Omni::Fiber::AsioApply([](auto err) -> auto { return err; })));

    if (stop) {
      break;
    }

    auto now = std::chrono::steady_clock::now();

    if (timerFired && !timerFired.value()) {
      auto condition = [now](const auto& item) -> auto { return item.second.IsExpired(now); };
      std::erase_if(_Ip4TcpTable, condition);
      std::erase_if(_Ip6TcpTable, condition);
      std::erase_if(_Ip4UdpTable, condition);
      std::erase_if(_Ip6UdpTable, condition);
      std::erase_if(_IcmpTable, condition);
      std::erase_if(_Icmp6Table, condition);
      timer.expires_after(ConnectionEntry::ProneInterval);
    }
  }
}

auto ConnectionTracker::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> {
  Clear();
  co_return ErrorCode{};
}

template <typename Direction>
auto ConnectionTracker::LookupAndUpdate(const Packet& packet, ConnectionTracker::Selector& selector) -> Result {
  auto now = std::chrono::steady_clock::now();
  return ParseConnectionKey<Direction>(
      packet.Data(), PacketType::kRealPacket, [&](auto&& key, PacketType type, auto keyExtra) -> Result {
        auto& table = (Overload{
            [this](const Ip4TcpKey& /*key*/) -> auto& { return _Ip4TcpTable; },
            [this](const Ip6TcpKey& /*key*/) -> auto& { return _Ip6TcpTable; },
            [this](const Ip4UdpKey& /*key*/) -> auto& { return _Ip4UdpTable; },
            [this](const Ip6UdpKey& /*key*/) -> auto& { return _Ip6UdpTable; },
            [this](const IcmpKey& /*key*/) -> auto& { return _IcmpTable; },
            [this](const Icmp6Key& /*key*/) -> auto& { return _Icmp6Table; },
        })(key);

        if (type == PacketType::kRealPacket) {
          using EntryType = typename std::decay_t<decltype(table)>::mapped_type;
          auto [iterator, inserted] = table.try_emplace(
              key, std::in_place_type<Direction>, [&] -> std::shared_ptr<ConnectionMark> { return selector(key); }, now,
              keyExtra);
          auto& entry = iterator->second;
          if (!inserted) {
            if (entry.IsExpired(now)) {
              entry = EntryType{std::in_place_type<Direction>,
                                [&] -> std::shared_ptr<ConnectionMark> { return selector(key); }, now, keyExtra};
            } else {
              if (!entry.ConnectionMark->Validate()) {
                entry.ConnectionMark = selector(key);
              }
              entry.LastActive = now;
              entry.template UpdateState<Direction>(keyExtra);
            }
          }

          return entry.ConnectionMark;
        } else {
          if (auto iterator = table.find(key); iterator != table.end() && iterator->second.Validate(now)) {
            return iterator->second.ConnectionMark;
          }
          return std::unexpected(Error(AppMinorErrorCategory::kUnsupportedPacket));
        }
      });
}

template auto ConnectionTracker::LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(
    const Packet& packet, ConnectionTracker::Selector& selector) -> Result;

template auto ConnectionTracker::LookupAndUpdate<ConnectionTracker::ConnectionDirectionInput>(
    const Packet& packet, ConnectionTracker::Selector& selector) -> Result;

void ConnectionTracker::Clear() {
  _Ip4TcpTable.clear();
  _Ip6TcpTable.clear();
  _Ip4UdpTable.clear();
  _Ip6UdpTable.clear();
  _IcmpTable.clear();
  _Icmp6Table.clear();
}

template <typename KeyDirection>
auto ConnectionTracker::ParseConnectionKey(std::span<const uint8_t> packet, PacketType type, auto&& function)
    -> Result {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* iph = reinterpret_cast<const IPHeader*>(packet.data());
  return iph->As(
      packet, type != PacketType::kRealPacket,
      Overload{
          [&](std::span<const uint8_t> ip4span, const IPv4Header* ip4) -> Result {
            auto srcAddr = boost::asio::ip::make_address_v4(ip4->GetSrcIp());
            auto dstAddr = boost::asio::ip::make_address_v4(ip4->GetDestIp());
            return ip4->Next(
                ip4span, type != PacketType::kRealPacket,
                Overload{
                    [&](std::span<const uint8_t> /*tcpspan*/, const TCPHeader* tcp) -> Result {
                      auto srcPort = tcp->GetSrcPort();
                      auto dstPort = tcp->GetDestPort();
                      TcpEntry::TcpExtraKey extra{.Flags = tcp->Flags,
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
                    [&](std::span<const uint8_t> /*udpspan*/, const UDPHeader* udp) -> Result {
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
                    [&](std::span<const uint8_t> icmpspan, const ICMPv4Header* icmp) -> Result {
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
                    [&](std::span<const uint8_t> /*span*/, std::string err) -> Result {
                      BOOST_LOG_TRIVIAL(info) << std::format("ConnectionTracker: {} -> {} {}", srcAddr.to_string(),
                                                             dstAddr.to_string(), err);
                      return std::unexpected(Error(AppMinorErrorCategory::kUnsupportedPacket));
                    },
                });
          },
          [&](std::span<const uint8_t> ip6span, const IPv6Header* ip6) -> Result {
            auto srcAddr = boost::asio::ip::make_address_v6(ip6->SrcIp);
            auto dstAddr = boost::asio::ip::make_address_v6(ip6->DestIp);
            return ip6->Next(
                ip6span, type != PacketType::kRealPacket,
                Overload{
                    [&](this auto& self, std::span<const uint8_t> hopByHopSpan, const IPv6HopByHopHeader* hopByHop)
                        -> Result { return hopByHop->Next(hopByHopSpan, type != PacketType::kRealPacket, self); },
                    [&](std::span<const uint8_t> /*tcpspan*/, const TCPHeader* tcp) -> Result {
                      auto srcPort = tcp->GetSrcPort();
                      auto dstPort = tcp->GetDestPort();
                      TcpEntry::TcpExtraKey extra{.Flags = tcp->Flags,
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
                    [&](std::span<const uint8_t> /*udpspan*/, const UDPHeader* udp) -> Result {
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
                    [&](std::span<const uint8_t> icmp6span, const ICMPv6Header* icmp6) -> Result {
                      if (icmp6->GetType() == Icmp6Type::EchoRequest || icmp6->GetType() == Icmp6Type::EchoReply) {
                        uint16_t icmp6Id = icmp6->GetEchoId();
                        if constexpr (std::is_same_v<KeyDirection, ConnectionDirectionOutput>) {
                          return function(Icmp6Key{.LocalAddress = srcAddr, .RemoteAddress = dstAddr, .Id = icmp6Id},
                                          type, Nothing{});
                        } else {
                          return function(Icmp6Key{.LocalAddress = dstAddr, .RemoteAddress = srcAddr, .Id = icmp6Id},
                                          type, Nothing{});
                        }
                      } else if (icmp6->GetType() == Icmp6Type::DestinationUnreachable) {
                        return ParseConnectionKey<typename KeyDirection::OppositeDirection>(
                            icmp6span.template subspan<sizeof(ICMPv6Header)>(), PacketType::kIcmpInnerPacket,
                            std::forward<decltype(function)>(function));
                      }
                      return std::unexpected(Error(AppMinorErrorCategory::kUnsupportedPacket));
                    },
                    [&](std::span<const uint8_t> /*span*/, std::string err) -> Result {
                      BOOST_LOG_TRIVIAL(info) << std::format("ConnectionTracker: {} -> {} {}", srcAddr.to_string(),
                                                             dstAddr.to_string(), err);
                      return std::unexpected(Error(AppMinorErrorCategory::kUnsupportedPacket));
                    },
                });
          },
          [&](std::span<const uint8_t> /*span*/, std::string err) -> Result {
            BOOST_LOG_TRIVIAL(info) << std::format("ConnectionTracker: {}", err);
            return std::unexpected(Error(AppMinorErrorCategory::kUnsupportedPacket));
          }});
}

} // namespace gh
