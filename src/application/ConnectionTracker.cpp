#include "ConnectionTracker.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

#include <boost/asio/ip/address_v6.hpp>
#include <boost/asio/steady_timer.hpp>
#include <utility>

#include "ErrorCode.hpp"
#include "PacketHeader.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"
#include "Utils/Overload.hpp"

namespace gh {

ConnectionTracker::ConnectionTracker(boost::asio::any_io_executor executor, Selector& selector)
    : _Executor(executor), _Selector(selector) {}

Omni::Fiber::Coroutine<ErrorCode> ConnectionTracker::DoStart() { co_return ErrorCode{}; }

Omni::Fiber::Coroutine<void> ConnectionTracker::DoWork() {
  boost::asio::steady_timer timer(_Executor);
  timer.expires_after(ConnectionEntry::ProneInterval);
  while (_State == State::kRunning && !_Service.value()._Stop.IsTriggered()) {
    auto [stop, timerFired] =
        co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(), [] {}),
                                     Omni::Fiber::SelectPair(timer.async_wait(_Service.value()._Stop.AsioSlot()()),
                                                             Omni::Fiber::AsioApply([](auto ec) { return ec; })));

    if (stop) {
      break;
    }

    auto now = std::chrono::steady_clock::now();

    if (timerFired && !timerFired.value()) {
      auto condition = [now](const auto& item) { return item.second.IsExpired(now); };
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

Omni::Fiber::Coroutine<ErrorCode> ConnectionTracker::DoGracefulStop() {
  Clear();
  co_return ErrorCode{};
}

template <typename Direction>
std::expected<std::reference_wrapper<ConnectionMark>, ErrorCode>
ConnectionTracker::LookupAndUpdate(const Packet& packet, ConnectionTracker::Selector& selector) {
  auto now = std::chrono::steady_clock::now();
  return ParseConnectionKey<Direction>(
      packet.Data(), PacketType::kRealPacket,
      [&](auto&& key, PacketType type,
          auto keyExtra) -> std::expected<std::reference_wrapper<ConnectionMark>, ErrorCode> {
        auto& table = (Overload{
            [this](const Ip4TcpKey& key) -> auto& { return _Ip4TcpTable; },
            [this](const Ip6TcpKey& key) -> auto& { return _Ip6TcpTable; },
            [this](const Ip4UdpKey& key) -> auto& { return _Ip4UdpTable; },
            [this](const Ip6UdpKey& key) -> auto& { return _Ip6UdpTable; },
            [this](const IcmpKey& key) -> auto& { return _IcmpTable; },
            [this](const Icmp6Key& key) -> auto& { return _Icmp6Table; },
        })(key);

        if (type == PacketType::kRealPacket) {
          using EntryType = typename std::decay_t<decltype(table)>::mapped_type;
          auto [it, inserted] =
              table.try_emplace(key, std::in_place_type<Direction>, [&] { return selector(key); }, now, keyExtra);
          auto& entry = it->second;
          if (!inserted) {
            if (entry.IsExpired(now)) {
              entry = EntryType{std::in_place_type<Direction>, [&] { return selector(key); }, now, keyExtra};
            } else {
              if (!entry.Result->Validate()) {
                entry.Result = selector(key);
              }
              entry.LastActive = now;
              entry.template UpdateState<Direction>(keyExtra);
            }
          }

          return std::reference_wrapper<ConnectionMark>(*entry.Result);
        } else {
          if (auto it = table.find(key); it != table.end() && it->second.Validate(now)) {
            return std::reference_wrapper<ConnectionMark>(*it->second.Result);
          }
          return std::unexpected(ErrorCode{AppMinorErrorCategory::kUnsupportedPacket, kAppError});
        }
      });
}

template std::expected<std::reference_wrapper<ConnectionMark>, ErrorCode>
ConnectionTracker::LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(const Packet& packet,
                                                                                 ConnectionTracker::Selector& selector);

template std::expected<std::reference_wrapper<ConnectionMark>, ErrorCode>
ConnectionTracker::LookupAndUpdate<ConnectionTracker::ConnectionDirectionInput>(const Packet& packet,
                                                                                ConnectionTracker::Selector& selector);

void ConnectionTracker::Clear() {
  _Ip4TcpTable.clear();
  _Ip6TcpTable.clear();
  _Ip4UdpTable.clear();
  _Ip6UdpTable.clear();
  _IcmpTable.clear();
  _Icmp6Table.clear();
}

template <typename KeyDirection, typename F>
std::expected<std::reference_wrapper<ConnectionMark>, ErrorCode>
ConnectionTracker::ParseConnectionKey(std::span<const uint8_t> p, PacketType type, F&& f) {
  using ReturnType = std::expected<std::reference_wrapper<ConnectionMark>, ErrorCode>;
  const IPHeader* iph = reinterpret_cast<const IPHeader*>(p.data());
  return iph->As(
      p, type != PacketType::kRealPacket,
      Overload{[&](std::span<const uint8_t> ip4span, const IPv4Header* ip4) -> ReturnType {
                 auto srcAddr = boost::asio::ip::make_address_v4(ip4->GetSrcIp());
                 auto dstAddr = boost::asio::ip::make_address_v4(ip4->GetDestIp());
                 return ip4->Next(
                     ip4span, type != PacketType::kRealPacket,
                     Overload{
                         [&](std::span<const uint8_t> tcpspan, const TCPHeader* tcp) -> ReturnType {
                           auto srcPort = tcp->GetSrcPort();
                           auto dstPort = tcp->GetDestPort();
                           TcpEntry::TcpExtraKey extra{.Flags = tcp->Flags,
                                                       .SequenceNumber = tcp->GetSeqNum(),
                                                       .AcknowledgementNumber = tcp->GetAckNum()};
                           if constexpr (std::is_same_v<KeyDirection, ConnectionDirectionOutput>) {
                             return f(Ip4TcpKey{srcAddr, dstAddr, srcPort, dstPort}, type, extra);
                           } else {
                             return f(Ip4TcpKey{dstAddr, srcAddr, dstPort, srcPort}, type, extra);
                           }
                         },
                         [&](std::span<const uint8_t> udpspan, const UDPHeader* udp) -> ReturnType {
                           auto srcPort = udp->GetSrcPort();
                           auto dstPort = udp->GetDestPort();
                           if constexpr (std::is_same_v<KeyDirection, ConnectionDirectionOutput>) {
                             return f(Ip4UdpKey{srcAddr, dstAddr, srcPort, dstPort}, type, Nothing{});
                           } else {
                             return f(Ip4UdpKey{dstAddr, srcAddr, dstPort, srcPort}, type, Nothing{});
                           }
                         },
                         [&](std::span<const uint8_t> icmpspan, const ICMPv4Header* icmp) -> ReturnType {
                           if (icmp->GetType() == IcmpType::EchoRequest || icmp->GetType() == IcmpType::EchoReply) {
                             uint16_t id = icmp->GetEchoId();
                             if constexpr (std::is_same_v<KeyDirection, ConnectionDirectionOutput>) {
                               return f(IcmpKey{srcAddr, dstAddr, id}, type, Nothing{});
                             } else {
                               return f(IcmpKey{dstAddr, srcAddr, id}, type, Nothing{});
                             }
                           } else if (icmp->GetType() == IcmpType::DestinationUnreachable) {
                             return ParseConnectionKey<typename KeyDirection::OppositeDirection>(
                                 icmpspan.template subspan<sizeof(ICMPv4Header)>(), PacketType::kIcmpInnerPacket,
                                 std::forward<F>(f));
                           }
                           return std::unexpected(ErrorCode{AppMinorErrorCategory::kUnsupportedPacket, kAppError});
                         },
                         [&](std::span<const uint8_t> span, std::string err) -> ReturnType {
                           BOOST_LOG_TRIVIAL(info) << std::format("ConnectionTracker: {} -> {} {}", srcAddr.to_string(),
                                                                  dstAddr.to_string(), err);
                           return std::unexpected(ErrorCode{AppMinorErrorCategory::kUnsupportedPacket, kAppError});
                         },
                     });
               },
               [&](std::span<const uint8_t> ip6span, const IPv6Header* ip6) -> ReturnType {
                 auto srcAddr = boost::asio::ip::make_address_v6(std::to_array(ip6->SrcIp));
                 auto dstAddr = boost::asio::ip::make_address_v6(std::to_array(ip6->DestIp));
                 return ip6->Next(
                     ip6span, type != PacketType::kRealPacket,
                     Overload{
                         [&](this auto& me, std::span<const uint8_t> hopByHopSpan,
                             const IPv6HopByHopHeader* hopByHop) -> ReturnType {
                           return hopByHop->Next(hopByHopSpan, type != PacketType::kRealPacket, me);
                         },
                         [&](std::span<const uint8_t> tcpspan, const TCPHeader* tcp) -> ReturnType {
                           auto srcPort = tcp->GetSrcPort();
                           auto dstPort = tcp->GetDestPort();
                           TcpEntry::TcpExtraKey extra{.Flags = tcp->Flags,
                                                       .SequenceNumber = tcp->GetSeqNum(),
                                                       .AcknowledgementNumber = tcp->GetAckNum()};
                           if constexpr (std::is_same_v<KeyDirection, ConnectionDirectionOutput>) {
                             return f(Ip6TcpKey{srcAddr, dstAddr, srcPort, dstPort}, type, extra);
                           } else {
                             return f(Ip6TcpKey{dstAddr, srcAddr, dstPort, srcPort}, type, extra);
                           }
                         },
                         [&](std::span<const uint8_t> udpspan, const UDPHeader* udp) -> ReturnType {
                           auto srcPort = udp->GetSrcPort();
                           auto dstPort = udp->GetDestPort();
                           if constexpr (std::is_same_v<KeyDirection, ConnectionDirectionOutput>) {
                             return f(Ip6UdpKey{srcAddr, dstAddr, srcPort, dstPort}, type, Nothing{});
                           } else {
                             return f(Ip6UdpKey{dstAddr, srcAddr, dstPort, srcPort}, type, Nothing{});
                           }
                         },
                         [&](std::span<const uint8_t> icmp6span, const ICMPv6Header* icmp6) -> ReturnType {
                           if (icmp6->GetType() == Icmp6Type::EchoRequest || icmp6->GetType() == Icmp6Type::EchoReply) {
                             uint16_t id = icmp6->GetEchoId();
                             if constexpr (std::is_same_v<KeyDirection, ConnectionDirectionOutput>) {
                               return f(Icmp6Key{srcAddr, dstAddr, id}, type, Nothing{});
                             } else {
                               return f(Icmp6Key{dstAddr, srcAddr, id}, type, Nothing{});
                             }
                           } else if (icmp6->GetType() == Icmp6Type::DestinationUnreachable) {
                             return ParseConnectionKey<typename KeyDirection::OppositeDirection>(
                                 icmp6span.template subspan<sizeof(ICMPv6Header)>(), PacketType::kIcmpInnerPacket,
                                 std::forward<F>(f));
                           }
                           return std::unexpected(ErrorCode{AppMinorErrorCategory::kUnsupportedPacket, kAppError});
                         },
                         [&](std::span<const uint8_t> span, std::string err) -> ReturnType {
                           BOOST_LOG_TRIVIAL(info) << std::format("ConnectionTracker: {} -> {} {}", srcAddr.to_string(),
                                                                  dstAddr.to_string(), err);
                           return std::unexpected(ErrorCode{AppMinorErrorCategory::kUnsupportedPacket, kAppError});
                         },
                     });
               },
               [&](std::span<const uint8_t> span, std::string err) -> ReturnType {
                 BOOST_LOG_TRIVIAL(info) << std::format("ConnectionTracker: {}", err);
                 return std::unexpected(ErrorCode{AppMinorErrorCategory::kUnsupportedPacket, kAppError});
               }});
}

} // namespace gh
