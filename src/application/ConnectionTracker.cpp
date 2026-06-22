#include "ConnectionTracker.hpp"

#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>

#include <boost/asio/ip/address_v6.hpp>
#include <boost/asio/steady_timer.hpp>

#include "PacketHeader.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"
#include "Utils/Overload.hpp"

namespace gh {

ConnectionTracker::ConnectionTracker(boost::asio::any_io_executor executor, SelectorType selector)
    : _Executor(executor), _Selector(selector) {}

Omni::Fiber::Coroutine<ErrorCode> ConnectionTracker::DoStart() { co_return ErrorCode{}; }

Omni::Fiber::Coroutine<void> ConnectionTracker::DoWork() {
  boost::asio::steady_timer timer(_Executor);
  timer.expires_after(std::chrono::seconds(5));
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
      auto condition = [this, now](const auto& item) { return now - item.Entry.LastActive > item.Entry.GetTimeout(); };
      std::erase_if(_Ip4TcpTable, condition);
      std::erase_if(_Ip6TcpTable, condition);
      std::erase_if(_Ip4UdpTable, condition);
      std::erase_if(_Ip6UdpTable, condition);
      std::erase_if(_IcmpTable, condition);
      std::erase_if(_Icmp6Table, condition);
      timer.expires_after(std::chrono::seconds(5));
    }
  }
}

Omni::Fiber::Coroutine<ErrorCode> ConnectionTracker::DoGracefulStop() {
  Clear();
  co_return ErrorCode{};
}

template <ConnectionDirection Direction>
std::optional<std::reference_wrapper<ConnectionMark>>
ConnectionTracker::LookupAndUpdate(const Packet& packet, std::optional<std::reference_wrapper<ConnectionMark>> mark,
                                   ValidatorType validator) {
  auto now = std::chrono::steady_clock::now();
  std::optional<std::reference_wrapper<ConnectionMark>> resultMark = std::nullopt;

  bool success = ParseConnectionKey<Direction>(packet.Data(), false, [&](auto&& key, auto keyExtra) -> bool {
    using T = std::decay_t<decltype(key)>;
    auto& table = [&]() -> auto& {
      if constexpr (std::is_same_v<T, Ip4TcpKey>) {
        return _Ip4TcpTable;
      } else if constexpr (std::is_same_v<T, Ip6TcpKey>) {
        return _Ip6TcpTable;
      } else if constexpr (std::is_same_v<T, Ip4UdpKey>) {
        return _Ip4UdpTable;
      } else if constexpr (std::is_same_v<T, Ip6UdpKey>) {
        return _Ip6UdpTable;
      } else if constexpr (std::is_same_v<T, IcmpKey>) {
        return _IcmpTable;
      } else if constexpr (std::is_same_v<T, Icmp6Key>) {
        return _Icmp6Table;
      }
    }();

    auto it = table.find(key);
    if (it != table.end()) {
      if (now - it->Entry.LastActive <= it->Entry.GetTimeout()) {
        auto& currentMark = it->Entry.Mark;
        if (validator && !validator(currentMark)) {
          table.erase(it);
        } else {
          it->Entry.LastActive = now;
          if constexpr (std::is_same_v<T, Ip4TcpKey> || std::is_same_v<T, Ip6TcpKey>) {
            it->Entry.UpdateTcpState(keyExtra);
          }
          resultMark = currentMark;
        }
      } else {
        table.erase(it);
      }
    }

    if (!resultMark.has_value()) {
      std::optional<std::reference_wrapper<ConnectionMark>> targetMark = std::nullopt;
      if (mark.has_value()) {
        if (!validator || validator(mark.value().get())) {
          targetMark = mark;
        }
      } else {
        auto selected = _Selector(key);
        if (selected.has_value()) {
          if (!validator || validator(selected.value().get())) {
            targetMark = selected;
          }
        }
      }

      if (targetMark.has_value()) {
        using EntryType = typename std::decay_t<decltype(table)>::value_type;
        if constexpr (std::is_same_v<T, Ip4TcpKey> || std::is_same_v<T, Ip6TcpKey>) {
          EntryType fullEntry{key, TcpEntry{targetMark.value().get(), now, TcpEntry::TcpState::kSynSent}};
          fullEntry.Entry.UpdateTcpState(keyExtra);
          table.emplace(fullEntry);
        } else if constexpr (std::is_same_v<T, Ip4UdpKey> || std::is_same_v<T, Ip6UdpKey>) {
          table.emplace(EntryType{key, UdpEntry{targetMark.value().get(), now}});
        } else {
          table.emplace(EntryType{key, IcmpConnEntry{targetMark.value().get(), now}});
        }
        resultMark = targetMark;
      }
    }
    return true;
  });

  if (!success) {
    return std::nullopt;
  }
  return resultMark;
}

template std::optional<std::reference_wrapper<ConnectionMark>>
ConnectionTracker::LookupAndUpdate<ConnectionDirection::kOutput>(
    const Packet& packet, std::optional<std::reference_wrapper<ConnectionMark>> mark, ValidatorType validator);

template std::optional<std::reference_wrapper<ConnectionMark>>
ConnectionTracker::LookupAndUpdate<ConnectionDirection::kInput>(
    const Packet& packet, std::optional<std::reference_wrapper<ConnectionMark>> mark, ValidatorType validator);

void ConnectionTracker::RemoveMark(const ConnectionMark& mark) {
  auto predicate = [&mark](const auto& item) { return &item.Entry.Mark == &mark; };
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

template <ConnectionDirection direction, typename F>
bool ConnectionTracker::ParseConnectionKey(std::span<const uint8_t> p, bool truncated, F&& f) {
  const IPHeader* iph = reinterpret_cast<const IPHeader*>(p.data());
  return iph->As(
      p, truncated,
      Overload{[&](std::span<const uint8_t> ip4span, const IPv4Header* ip4) {
                 auto srcAddr = boost::asio::ip::make_address_v4(ip4->GetSrcIp());
                 auto dstAddr = boost::asio::ip::make_address_v4(ip4->GetDestIp());
                 return ip4->Next(
                     ip4span, truncated,
                     Overload{
                         [&](std::span<const uint8_t> tcpspan, const TCPHeader* tcp) -> bool {
                           auto srcPort = tcp->GetSrcPort();
                           auto dstPort = tcp->GetDestPort();
                           if (direction == ConnectionDirection::kOutput) {
                             return f(Ip4TcpKey{srcAddr, dstAddr, srcPort, dstPort}, tcp->Flags);
                           } else {
                             return f(Ip4TcpKey{dstAddr, srcAddr, dstPort, srcPort}, tcp->Flags);
                           }
                         },
                         [&](std::span<const uint8_t> udpspan, const UDPHeader* udp) -> bool {
                           auto srcPort = udp->GetSrcPort();
                           auto dstPort = udp->GetDestPort();
                           if (direction == ConnectionDirection::kOutput) {
                             return f(Ip4UdpKey{srcAddr, dstAddr, srcPort, dstPort}, 0);
                           } else {
                             return f(Ip4UdpKey{dstAddr, srcAddr, dstPort, srcPort}, 0);
                           }
                         },
                         [&](std::span<const uint8_t> icmpspan, const ICMPv4Header* icmp) -> bool {
                           if (icmp->GetType() == IcmpType::EchoRequest || icmp->GetType() == IcmpType::EchoReply) {
                             uint16_t id = icmp->GetEchoId();
                             if (direction == ConnectionDirection::kOutput) {
                               return f(IcmpKey{srcAddr, dstAddr, id}, 0);
                             } else {
                               return f(IcmpKey{dstAddr, srcAddr, id}, 0);
                             }
                           } else if (icmp->GetType() == IcmpType::DestinationUnreachable) {
                             if constexpr (direction == ConnectionDirection::kOutput) {
                               return ParseConnectionKey<ConnectionDirection::kInput>(
                                   icmpspan.template subspan<sizeof(ICMPv4Header)>(), true, std::forward<F>(f));
                             } else {
                               return ParseConnectionKey<ConnectionDirection::kOutput>(
                                   icmpspan.template subspan<sizeof(ICMPv4Header)>(), true, std::forward<F>(f));
                             }
                           }
                           return false;
                         },
                         [&](std::span<const uint8_t> span) -> bool { return false; },
                     });
               },
               [&](std::span<const uint8_t> ip6span, const IPv6Header* ip6) {
                 auto srcAddr = boost::asio::ip::make_address_v6(std::to_array(ip6->SrcIp));
                 auto dstAddr = boost::asio::ip::make_address_v6(std::to_array(ip6->DestIp));
                 return ip6->Next(
                     ip6span, truncated,
                     Overload{
                         [&](this auto& me, std::span<const uint8_t> hopByHopSpan, const IPv6HopByHopHeader* hopByHop)
                             -> bool { return hopByHop->Next(hopByHopSpan, truncated, me); },
                         [&](std::span<const uint8_t> tcpspan, const TCPHeader* tcp) {
                           auto srcPort = tcp->GetSrcPort();
                           auto dstPort = tcp->GetDestPort();
                           if (direction == ConnectionDirection::kOutput) {
                             return f(Ip6TcpKey{srcAddr, dstAddr, srcPort, dstPort}, tcp->Flags);
                           } else {
                             return f(Ip6TcpKey{dstAddr, srcAddr, dstPort, srcPort}, tcp->Flags);
                           }
                         },
                         [&](std::span<const uint8_t> udpspan, const UDPHeader* udp) {
                           auto srcPort = udp->GetSrcPort();
                           auto dstPort = udp->GetDestPort();
                           if (direction == ConnectionDirection::kOutput) {
                             return f(Ip6UdpKey{srcAddr, dstAddr, srcPort, dstPort}, 0);
                           } else {
                             return f(Ip6UdpKey{dstAddr, srcAddr, dstPort, srcPort}, 0);
                           }
                         },
                         [&](std::span<const uint8_t> icmp6span, const ICMPv6Header* icmp6) {
                           if (icmp6->GetType() == Icmp6Type::EchoRequest || icmp6->GetType() == Icmp6Type::EchoReply) {
                             uint16_t id = icmp6->GetEchoId();
                             if (direction == ConnectionDirection::kOutput) {
                               return f(Icmp6Key{srcAddr, dstAddr, id}, 0);
                             } else {
                               return f(Icmp6Key{dstAddr, srcAddr, id}, 0);
                             }
                           } else if (icmp6->GetType() == Icmp6Type::DestinationUnreachable) {
                             if constexpr (direction == ConnectionDirection::kOutput) {
                               return ParseConnectionKey<ConnectionDirection::kInput>(
                                   icmp6span.template subspan<sizeof(ICMPv6Header)>(), true, std::forward<F>(f));
                             } else {
                               return ParseConnectionKey<ConnectionDirection::kOutput>(
                                   icmp6span.template subspan<sizeof(ICMPv6Header)>(), true, std::forward<F>(f));
                             }
                           }
                           return false;
                         },
                         [&](std::span<const uint8_t> span) -> bool { return false; },
                     });
               },
               [&](std::span<const uint8_t> span) { return false; }});
}

} // namespace gh
