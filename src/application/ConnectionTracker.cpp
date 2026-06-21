#include "ConnectionTracker.hpp"

#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <set>

#include <boost/asio/ip/address_v6.hpp>
#include <boost/asio/steady_timer.hpp>

#include "Select.hpp"
#include "SelectPair.hpp"

namespace gh {

template <class... Ts> struct overloaded : Ts... {
  using Ts::operator()...;
};

ConnectionTracker::ConnectionTracker(boost::asio::any_io_executor executor, SelectorType selector)
    : _Executor(executor), _Selector(selector) {}

Omni::Fiber::Coroutine<ErrorCode> ConnectionTracker::DoStart() { co_return ErrorCode{}; }

Omni::Fiber::Coroutine<void> ConnectionTracker::DoWork() {
  boost::asio::steady_timer tcpTimer(_Executor);
  boost::asio::steady_timer udpTimer(_Executor);
  boost::asio::steady_timer icmpTimer(_Executor);

  tcpTimer.expires_after(std::chrono::seconds(5));
  udpTimer.expires_after(std::chrono::seconds(5));
  icmpTimer.expires_after(std::chrono::seconds(5));

  while (_State == State::kRunning && !_Service.value()._Stop.IsTriggered()) {
    auto [stop, tcpFired, udpFired, icmpFired] =
        co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(), [] {}),
                                     Omni::Fiber::SelectPair(tcpTimer.async_wait(_Service.value()._Stop.AsioSlot()()),
                                                             Omni::Fiber::AsioApply([](auto ec) { return ec; })),
                                     Omni::Fiber::SelectPair(udpTimer.async_wait(_Service.value()._Stop.AsioSlot()()),
                                                             Omni::Fiber::AsioApply([](auto ec) { return ec; })),
                                     Omni::Fiber::SelectPair(icmpTimer.async_wait(_Service.value()._Stop.AsioSlot()()),
                                                             Omni::Fiber::AsioApply([](auto ec) { return ec; })));

    if (stop) {
      break;
    }

    auto now = std::chrono::steady_clock::now();

    if (tcpFired && !tcpFired.value()) {
      std::erase_if(_Ip4TcpTable, [this, now](const auto& item) {
        std::chrono::seconds timeout = TcpEntry::FinTimeout;
        if (item.Entry.State == TcpState::kSynSent) {
          timeout = TcpEntry::SynTimeout;
        } else if (item.Entry.State == TcpState::kEstablished) {
          timeout = TcpEntry::EstablishedTimeout;
        }
        return now - item.Entry.LastActive > timeout;
      });
      std::erase_if(_Ip6TcpTable, [this, now](const auto& item) {
        std::chrono::seconds timeout = TcpEntry::FinTimeout;
        if (item.Entry.State == TcpState::kSynSent) {
          timeout = TcpEntry::SynTimeout;
        } else if (item.Entry.State == TcpState::kEstablished) {
          timeout = TcpEntry::EstablishedTimeout;
        }
        return now - item.Entry.LastActive > timeout;
      });
      tcpTimer.expires_after(std::chrono::seconds(5));
    }

    if (udpFired && !udpFired.value()) {
      auto predicate = [this, now](const auto& item) { return now - item.Entry.LastActive > UdpEntry::Timeout; };
      std::erase_if(_Ip4UdpTable, predicate);
      std::erase_if(_Ip6UdpTable, predicate);
      udpTimer.expires_after(std::chrono::seconds(5));
    }

    if (icmpFired && !icmpFired.value()) {
      auto predicate = [this, now](const auto& item) { return now - item.Entry.LastActive > IcmpConnEntry::Timeout; };
      std::erase_if(_IcmpTable, predicate);
      std::erase_if(_Icmp6Table, predicate);
      icmpTimer.expires_after(std::chrono::seconds(5));
    }
  }
}

Omni::Fiber::Coroutine<ErrorCode> ConnectionTracker::DoGracefulStop() {
  Clear();
  co_return ErrorCode{};
}

void ConnectionTracker::Update(const Packet& packet, ConnectionMark& mark, ConnectionDirection direction) {
  auto keyOpt = ParseConnectionKey(packet, direction);
  if (!keyOpt) {
    return;
  }

  std::visit(
      [&](auto&& k) {
        using T = std::decay_t<decltype(k)>;
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

        using EntryType = typename std::decay_t<decltype(table)>::value_type;

        auto it = table.find(k);
        auto now = std::chrono::steady_clock::now();
        if (it != table.end()) {
          it->Entry.LastActive = now;
          if constexpr (std::is_same_v<T, Ip4TcpKey> || std::is_same_v<T, Ip6TcpKey>) {
            if (auto flags = GetTcpFlags(packet)) {
              UpdateTcpState(it->Entry, *flags);
            }
          }
        } else {
          if constexpr (std::is_same_v<T, Ip4TcpKey> || std::is_same_v<T, Ip6TcpKey>) {
            EntryType fullEntry{k, TcpEntry{mark, now, TcpState::kSynSent}};
            if (auto flags = GetTcpFlags(packet)) {
              UpdateTcpState(fullEntry.Entry, *flags);
            }
            table.emplace(fullEntry);
          } else if constexpr (std::is_same_v<T, Ip4UdpKey> || std::is_same_v<T, Ip6UdpKey>) {
            table.emplace(EntryType{k, UdpEntry{mark, now}});
          } else {
            table.emplace(EntryType{k, IcmpConnEntry{mark, now}});
          }
        }
      },
      *keyOpt);
}

std::optional<std::reference_wrapper<ConnectionMark>>
ConnectionTracker::Lookup(const Packet& packet, ConnectionDirection direction, ValidatorType validator) {
  auto keyOpt = ParseConnectionKey(packet, direction);
  if (!keyOpt) {
    return std::nullopt;
  }

  auto now = std::chrono::steady_clock::now();
  std::optional<std::reference_wrapper<ConnectionMark>> mark = std::nullopt;

  std::visit(
      [&](auto&& k) {
        using T = std::decay_t<decltype(k)>;
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

        using EntryType = typename std::decay_t<decltype(table)>::value_type;

        auto it = table.find(k);
        if (it != table.end()) {
          std::chrono::seconds timeout = UdpEntry::Timeout;
          if constexpr (std::is_same_v<T, Ip4TcpKey> || std::is_same_v<T, Ip6TcpKey>) {
            timeout = TcpEntry::FinTimeout;
            if (it->Entry.State == TcpState::kSynSent) {
              timeout = TcpEntry::SynTimeout;
            } else if (it->Entry.State == TcpState::kEstablished) {
              timeout = TcpEntry::EstablishedTimeout;
            }
          } else if constexpr (std::is_same_v<T, Ip4UdpKey> || std::is_same_v<T, Ip6UdpKey>) {
            timeout = UdpEntry::Timeout;
          } else {
            timeout = IcmpConnEntry::Timeout;
          }

          if (now - it->Entry.LastActive <= timeout) {
            mark = it->Entry.Mark;
            if (validator && !validator(mark->get())) {
              table.erase(it);
              mark = std::nullopt;
            } else {
              it->Entry.LastActive = now;
              if constexpr (std::is_same_v<T, Ip4TcpKey> || std::is_same_v<T, Ip6TcpKey>) {
                if (auto flags = GetTcpFlags(packet)) {
                  UpdateTcpState(it->Entry, *flags);
                }
              }
            }
          } else {
            table.erase(it);
          }
        }
      },
      *keyOpt);

  if (!mark.has_value()) {
    auto selected = std::visit(_Selector, *keyOpt);
    if (selected.has_value()) {
      if (!validator || validator(selected.value().get())) {
        std::visit(
            [&](auto&& k) {
              using T = std::decay_t<decltype(k)>;
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

              using EntryType = typename std::decay_t<decltype(table)>::value_type;

              if constexpr (std::is_same_v<T, Ip4TcpKey> || std::is_same_v<T, Ip6TcpKey>) {
                EntryType fullEntry{k, TcpEntry{selected.value(), now, TcpState::kSynSent}};
                if (auto flags = GetTcpFlags(packet)) {
                  UpdateTcpState(fullEntry.Entry, *flags);
                }
                table.emplace(fullEntry);
              } else if constexpr (std::is_same_v<T, Ip4UdpKey> || std::is_same_v<T, Ip6UdpKey>) {
                table.emplace(EntryType{k, UdpEntry{selected.value(), now}});
              } else {
                table.emplace(EntryType{k, IcmpConnEntry{selected.value(), now}});
              }
            },
            *keyOpt);
        mark = std::move(selected);
      }
    }
  }

  return mark;
}

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

std::optional<ConnectionTracker::ConnectionKey> ConnectionTracker::ParseConnectionKey(const Packet& p,
                                                                                      ConnectionDirection direction) {
  if (p.DataSize() < 20) {
    return std::nullopt;
  }
  uint8_t version = (p.Data()[0] >> 4);
  if (version == 4) {
    uint8_t ihl = (p.Data()[0] & 0x0F) * 4;
    if (p.DataSize() < ihl) {
      return std::nullopt;
    }
    uint8_t protocol = p.Data()[9];
    std::array<uint8_t, 4> srcBytes;
    std::copy_n(p.Data().data() + 12, 4, srcBytes.begin());
    std::array<uint8_t, 4> dstBytes;
    std::copy_n(p.Data().data() + 16, 4, dstBytes.begin());
    auto srcAddr = boost::asio::ip::make_address_v4(srcBytes);
    auto dstAddr = boost::asio::ip::make_address_v4(dstBytes);

    uint16_t srcPort = 0;
    uint16_t dstPort = 0;
    if ((protocol == 6 || protocol == 17) && p.DataSize() >= ihl + 4) {
      srcPort = (p.Data()[ihl] << 8) | p.Data()[ihl + 1];
      dstPort = (p.Data()[ihl + 2] << 8) | p.Data()[ihl + 3];
    } else if (protocol == 1 && p.DataSize() >= ihl + 8) {
      uint8_t type = p.Data()[ihl];
      if (type == 3) {
        size_t innerOffset = ihl + 8;
        if (p.DataSize() > innerOffset) {
          size_t innerSize = p.DataSize() - innerOffset;
          Packet inner(innerSize, 0);
          std::copy(p.Data().begin() + innerOffset, p.Data().end(), inner.Data().begin());
          auto oppositeDirection =
              (direction == ConnectionDirection::kOutput) ? ConnectionDirection::kInput : ConnectionDirection::kOutput;
          return ParseConnectionKey(inner, oppositeDirection);
        }
      } else if (type == 8 || type == 0) {
        uint16_t id = (p.Data()[ihl + 4] << 8) | p.Data()[ihl + 5];
        srcPort = id;
        dstPort = id;
      }
    }

    if (protocol == 6) {
      if (direction == ConnectionDirection::kOutput) {
        return Ip4TcpKey{srcAddr, dstAddr, srcPort, dstPort};
      } else {
        return Ip4TcpKey{dstAddr, srcAddr, dstPort, srcPort};
      }
    } else if (protocol == 17) {
      if (direction == ConnectionDirection::kOutput) {
        return Ip4UdpKey{srcAddr, dstAddr, srcPort, dstPort};
      } else {
        return Ip4UdpKey{dstAddr, srcAddr, dstPort, srcPort};
      }
    } else if (protocol == 1) {
      uint16_t id = srcPort;
      if (direction == ConnectionDirection::kOutput) {
        return IcmpKey{srcAddr, dstAddr, id};
      } else {
        return IcmpKey{dstAddr, srcAddr, id};
      }
    }
  } else if (version == 6) {
    if (p.DataSize() < 40) {
      return std::nullopt;
    }
    std::array<uint8_t, 16> srcBytes;
    std::copy_n(p.Data().data() + 8, 16, srcBytes.begin());
    std::array<uint8_t, 16> dstBytes;
    std::copy_n(p.Data().data() + 24, 16, dstBytes.begin());
    auto srcAddr = boost::asio::ip::make_address_v6(srcBytes);
    auto dstAddr = boost::asio::ip::make_address_v6(dstBytes);

    uint8_t nextHeader = p.Data()[6];
    size_t offset = 40;
    uint16_t srcPort = 0;
    uint16_t dstPort = 0;
    uint8_t protocol = nextHeader;

    while (true) {
      if (nextHeader == 6 || nextHeader == 17) {
        if (p.DataSize() >= offset + 4) {
          srcPort = (p.Data()[offset] << 8) | p.Data()[offset + 1];
          dstPort = (p.Data()[offset + 2] << 8) | p.Data()[offset + 3];
          protocol = nextHeader;
        }
        break;
      }
      if (nextHeader == 58) {
        if (p.DataSize() >= offset + 8) {
          uint8_t type = p.Data()[offset];
          if (type == 1) {
            size_t innerOffset = offset + 8;
            if (p.DataSize() > innerOffset) {
              size_t innerSize = p.DataSize() - innerOffset;
              Packet inner(innerSize, 0);
              std::copy(p.Data().begin() + innerOffset, p.Data().end(), inner.Data().begin());
              auto oppositeDirection = (direction == ConnectionDirection::kOutput) ? ConnectionDirection::kInput
                                                                                   : ConnectionDirection::kOutput;
              return ParseConnectionKey(inner, oppositeDirection);
            }
          } else if (type == 128 || type == 129) {
            uint16_t id = (p.Data()[offset + 4] << 8) | p.Data()[offset + 5];
            srcPort = id;
            dstPort = id;
          }
          protocol = nextHeader;
        }
        break;
      }
      // Extension headers
      if (nextHeader == 0 || nextHeader == 43 || nextHeader == 60) {
        if (p.DataSize() < offset + 2) {
          break;
        }
        uint8_t extLen = p.Data()[offset + 1];
        nextHeader = p.Data()[offset];
        offset += (extLen + 1) * 8;
      } else if (nextHeader == 44) {
        if (p.DataSize() < offset + 8) {
          break;
        }
        nextHeader = p.Data()[offset];
        offset += 8;
      } else {
        protocol = nextHeader;
        break;
      }
    }

    if (protocol == 6) {
      if (direction == ConnectionDirection::kOutput) {
        return Ip6TcpKey{srcAddr, dstAddr, srcPort, dstPort};
      } else {
        return Ip6TcpKey{dstAddr, srcAddr, dstPort, srcPort};
      }
    } else if (protocol == 17) {
      if (direction == ConnectionDirection::kOutput) {
        return Ip6UdpKey{srcAddr, dstAddr, srcPort, dstPort};
      } else {
        return Ip6UdpKey{dstAddr, srcAddr, dstPort, srcPort};
      }
    } else if (protocol == 58) {
      uint16_t id = srcPort;
      if (direction == ConnectionDirection::kOutput) {
        return Icmp6Key{srcAddr, dstAddr, id};
      } else {
        return Icmp6Key{dstAddr, srcAddr, id};
      }
    }
  }
  return std::nullopt;
}

std::optional<uint8_t> ConnectionTracker::GetTcpFlags(const Packet& p) {
  if (p.DataSize() < 20) {
    return std::nullopt;
  }
  uint8_t version = (p.Data()[0] >> 4);
  if (version == 4) {
    uint8_t ihl = (p.Data()[0] & 0x0F) * 4;
    if (p.DataSize() < ihl) {
      return std::nullopt;
    }
    uint8_t protocol = p.Data()[9];
    if (protocol == 6 && p.DataSize() >= ihl + 14) {
      return p.Data()[ihl + 13];
    }
  } else if (version == 6) {
    if (p.DataSize() < 40) {
      return std::nullopt;
    }
    uint8_t nextHeader = p.Data()[6];
    size_t offset = 40;
    while (true) {
      if (nextHeader == 6) {
        if (p.DataSize() >= offset + 14) {
          return p.Data()[offset + 13];
        }
        break;
      }
      if (nextHeader == 17 || nextHeader == 58) {
        break;
      }
      if (nextHeader == 0 || nextHeader == 43 || nextHeader == 60) {
        if (p.DataSize() < offset + 2) {
          break;
        }
        uint8_t extLen = p.Data()[offset + 1];
        nextHeader = p.Data()[offset];
        offset += (extLen + 1) * 8;
      } else if (nextHeader == 44) {
        if (p.DataSize() < offset + 8) {
          break;
        }
        nextHeader = p.Data()[offset];
        offset += 8;
      } else {
        break;
      }
    }
  }
  return std::nullopt;
}

} // namespace gh
