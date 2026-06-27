#include "ConnectionTracker.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>

#include <boost/asio/ip/address_v6.hpp>
#include <boost/asio/steady_timer.hpp>
#include <utility>

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
      auto condition = [now](const auto& item) { return IsExpired(item.Entry, now); };
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
typename Direction::ConnectionTrackerOutput ConnectionTracker::LookupAndUpdate(const Packet& packet,
                                                                               Direction::ConnectionTrackerInput input,
                                                                               ValidatorType validator) {
  auto now = std::chrono::steady_clock::now();
  auto res = ParseConnectionKey<Direction, typename Direction::ConnectionTrackerOutput>(
      packet.Data(), PacketType::kRealPacket,
      [&](auto&& key, PacketType type, auto keyExtra) -> Direction::ConnectionTrackerOutput {
        auto& table = (Overload{
            [this](const Ip4TcpKey& key) -> auto& { return _Ip4TcpTable; },
            [this](const Ip6TcpKey& key) -> auto& { return _Ip6TcpTable; },
            [this](const Ip4UdpKey& key) -> auto& { return _Ip4UdpTable; },
            [this](const Ip6UdpKey& key) -> auto& { return _Ip6UdpTable; },
            [this](const IcmpKey& key) -> auto& { return _IcmpTable; },
            [this](const Icmp6Key& key) -> auto& { return _Icmp6Table; },
        })(key);

        if (type == PacketType::kRealPacket) {
          RouteResult resetRoute = ([&]() {
            if constexpr (std::is_same_v<Direction, ConnectionDirectionOutput>) {
              return RouteResult{ToBeSelected{}};
            } else {
              return RouteResult{input};
            }
          })();

          using TableValueType = typename std::decay_t<decltype(table)>::value_type;
          using EntryType = decltype(std::declval<TableValueType>().Entry);
          auto [it, inserted] =
              table.insert({key, EntryType{std::in_place_type<Direction>, resetRoute, now, keyExtra}});
          auto& entry = it->Entry;
          if (!inserted) {
            if (IsExpired(entry, now)) {
              entry = EntryType{std::in_place_type<Direction>, resetRoute, now, keyExtra};
            } else {
              if constexpr (std::is_same_v<Direction, ConnectionDirectionInput>) {
                entry.Result = resetRoute;
              } else {
                if (std::holds_alternative<std::reference_wrapper<ConnectionMark>>(entry.Result)) {
                  if (validator && !validator(std::get<std::reference_wrapper<ConnectionMark>>(entry.Result).get())) {
                    entry.Result = resetRoute;
                  }
                }
              }
              entry.LastActive = now;
              entry.template UpdateState<Direction>(keyExtra);
            }
          }

          if constexpr (std::is_same_v<Direction, ConnectionDirectionOutput>) {
            if (std::holds_alternative<ToBeSelected>(entry.Result)) {
              entry.Result = _Selector(key);
            }
          }

          if constexpr (std::is_same_v<Direction, ConnectionDirectionOutput>) {
            return entry.Result;
          } else {
            return Nothing{};
          }
        } else {
          if constexpr (std::is_same_v<Direction, ConnectionDirectionOutput>) {
            if (auto it = table.find(key); it != table.end() && !IsExpired(it->Entry, now)) {
              return it->Entry.Result;
            } else {
              return RouteResult{Discard{}};
            }
          } else {
            return Nothing{};
          }
        }
      });

  if constexpr (std::is_same_v<Direction, ConnectionDirectionOutput>) {
    return res.value_or(Discard{});
  } else {
    return Nothing{};
  }
}

template typename ConnectionTracker::ConnectionDirectionOutput::ConnectionTrackerOutput
ConnectionTracker::LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(
    const Packet& packet, ConnectionTracker::ConnectionDirectionOutput::ConnectionTrackerInput input,
    ValidatorType validator);

template typename ConnectionTracker::ConnectionDirectionInput::ConnectionTrackerOutput
ConnectionTracker::LookupAndUpdate<ConnectionTracker::ConnectionDirectionInput>(
    const Packet& packet, ConnectionTracker::ConnectionDirectionInput::ConnectionTrackerInput input,
    ValidatorType validator);

void ConnectionTracker::RemoveMark(const ConnectionMark& mark) {
  auto predicate = [&mark](const auto& item) {
    if (std::holds_alternative<std::reference_wrapper<ConnectionMark>>(item.Entry.Result)) {
      return &std::get<std::reference_wrapper<ConnectionMark>>(item.Entry.Result).get() == &mark;
    }
    return false;
  };
  std::erase_if(_Ip4TcpTable, predicate);
  std::erase_if(_Ip6TcpTable, predicate);
  std::erase_if(_Ip4UdpTable, predicate);
  std::erase_if(_Ip6UdpTable, predicate);
  std::erase_if(_IcmpTable, predicate);
  std::erase_if(_Icmp6Table, predicate);
}

void ConnectionTracker::Clear() {
  _Ip4TcpTable.clear();
  _Ip6TcpTable.clear();
  _Ip4UdpTable.clear();
  _Ip6UdpTable.clear();
  _IcmpTable.clear();
  _Icmp6Table.clear();
}

template <typename KeyDirection, typename Result, typename F>
std::expected<Result, ConnectionTracker::UnsupportedPacket>
ConnectionTracker::ParseConnectionKey(std::span<const uint8_t> p, PacketType type, F&& f) {
  using ReturnType = std::expected<Result, UnsupportedPacket>;
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
                             return ParseConnectionKey<typename KeyDirection::OppositeDirection, Result>(
                                 icmpspan.template subspan<sizeof(ICMPv4Header)>(), PacketType::kIcmpInnerPacket,
                                 std::forward<F>(f));
                           }
                           return std::unexpected(UnsupportedPacket{});
                         },
                         [&](std::span<const uint8_t> span, std::string err) -> ReturnType {
                           BOOST_LOG_TRIVIAL(info) << std::format("ConnectionTracker: {} -> {} {}", srcAddr.to_string(),
                                                                  dstAddr.to_string(), err);
                           return std::unexpected(UnsupportedPacket{});
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
                             return ParseConnectionKey<typename KeyDirection::OppositeDirection, Result>(
                                 icmp6span.template subspan<sizeof(ICMPv6Header)>(), PacketType::kIcmpInnerPacket,
                                 std::forward<F>(f));
                           }
                           return std::unexpected(UnsupportedPacket{});
                         },
                         [&](std::span<const uint8_t> span, std::string err) -> ReturnType {
                           BOOST_LOG_TRIVIAL(info) << std::format("ConnectionTracker: {} -> {} {}", srcAddr.to_string(),
                                                                  dstAddr.to_string(), err);
                           return std::unexpected(UnsupportedPacket{});
                         },
                     });
               },
               [&](std::span<const uint8_t> span, std::string err) -> ReturnType {
                 BOOST_LOG_TRIVIAL(info) << std::format("ConnectionTracker: {}", err);
                 return std::unexpected(UnsupportedPacket{});
               }});
}

} // namespace gh
