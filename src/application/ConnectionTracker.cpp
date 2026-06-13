#include "ConnectionTracker.hpp"

#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <optional>

#include <boost/asio/ip/address_v6.hpp>

#include "Utils.hpp"

namespace gh {

ConnectionTracker::ConnectionTracker(std::chrono::seconds timeout) : _Timeout(timeout) {}

void ConnectionTracker::Update(const Packet& packet, std::shared_ptr<Endpoint> channel, ConnectionDirection direction) {
  auto keyOpt = ParseConnectionKey(packet, direction);
  if (!keyOpt) {
    return;
  }

  std::visit(
      [&](auto&& k) {
        using T = std::decay_t<decltype(k)>;
        auto entry = ConnectionEntry{std::move(channel), std::chrono::steady_clock::now()};
        if constexpr (std::is_same_v<T, Ip4TcpKey>) {
          _Ip4TcpTable[k] = std::move(entry);
        } else if constexpr (std::is_same_v<T, Ip6TcpKey>) {
          _Ip6TcpTable[k] = std::move(entry);
        } else if constexpr (std::is_same_v<T, Ip4UdpKey>) {
          _Ip4UdpTable[k] = std::move(entry);
        } else if constexpr (std::is_same_v<T, Ip6UdpKey>) {
          _Ip6UdpTable[k] = std::move(entry);
        } else if constexpr (std::is_same_v<T, IcmpKey>) {
          _IcmpTable[k] = std::move(entry);
        } else if constexpr (std::is_same_v<T, Icmp6Key>) {
          _Icmp6Table[k] = std::move(entry);
        }
      },
      *keyOpt);
}

std::shared_ptr<Endpoint> ConnectionTracker::Lookup(const Packet& packet, ConnectionDirection direction,
                                                    ValidatorType validator, SelectorType selector) {
  auto keyOpt = ParseConnectionKey(packet, direction);
  if (!keyOpt) {
    return nullptr;
  }

  auto now = std::chrono::steady_clock::now();
  std::shared_ptr<Endpoint> channel = nullptr;

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

        auto it = table.find(k);
        if (it != table.end()) {
          if (now - it->second.LastActive <= _Timeout) {
            channel = it->second.Channel;
            if (validator && !validator(channel)) {
              table.erase(it);
              channel = nullptr;
            } else {
              it->second.LastActive = now;
            }
          } else {
            table.erase(it);
          }
        }
      },
      *keyOpt);

  if (!channel && selector) {
    boost::asio::ip::address_v6 src;
    boost::asio::ip::address_v6 dst;
    uint16_t srcPort = 0;
    uint16_t dstPort = 0;
    uint8_t protocol = 0;

    std::visit(
        [&](auto&& k) {
          using T = std::decay_t<decltype(k)>;
          if constexpr (std::is_same_v<T, Ip4TcpKey>) {
            src = MapToV6(k.LocalAddress);
            dst = MapToV6(k.RemoteAddress);
            srcPort = k.LocalPort;
            dstPort = k.RemotePort;
            protocol = 6;
          } else if constexpr (std::is_same_v<T, Ip6TcpKey>) {
            src = k.LocalAddress;
            dst = k.RemoteAddress;
            srcPort = k.LocalPort;
            dstPort = k.RemotePort;
            protocol = 6;
          } else if constexpr (std::is_same_v<T, Ip4UdpKey>) {
            src = MapToV6(k.LocalAddress);
            dst = MapToV6(k.RemoteAddress);
            srcPort = k.LocalPort;
            dstPort = k.RemotePort;
            protocol = 17;
          } else if constexpr (std::is_same_v<T, Ip6UdpKey>) {
            src = k.LocalAddress;
            dst = k.RemoteAddress;
            srcPort = k.LocalPort;
            dstPort = k.RemotePort;
            protocol = 17;
          } else if constexpr (std::is_same_v<T, IcmpKey>) {
            src = MapToV6(k.LocalAddress);
            dst = MapToV6(k.RemoteAddress);
            srcPort = k.Id;
            dstPort = k.Id;
            protocol = 1;
          } else if constexpr (std::is_same_v<T, Icmp6Key>) {
            src = k.LocalAddress;
            dst = k.RemoteAddress;
            srcPort = k.Id;
            dstPort = k.Id;
            protocol = 58;
          }
        },
        *keyOpt);

    auto selected = selector(src, dst, srcPort, dstPort, protocol);
    if (selected) {
      if (!validator || validator(selected)) {
        std::visit(
            [&](auto&& k) {
              using T = std::decay_t<decltype(k)>;
              auto entry = ConnectionEntry{selected, now};
              if constexpr (std::is_same_v<T, Ip4TcpKey>) {
                _Ip4TcpTable[k] = std::move(entry);
              } else if constexpr (std::is_same_v<T, Ip6TcpKey>) {
                _Ip6TcpTable[k] = std::move(entry);
              } else if constexpr (std::is_same_v<T, Ip4UdpKey>) {
                _Ip4UdpTable[k] = std::move(entry);
              } else if constexpr (std::is_same_v<T, Ip6UdpKey>) {
                _Ip6UdpTable[k] = std::move(entry);
              } else if constexpr (std::is_same_v<T, IcmpKey>) {
                _IcmpTable[k] = std::move(entry);
              } else if constexpr (std::is_same_v<T, Icmp6Key>) {
                _Icmp6Table[k] = std::move(entry);
              }
            },
            *keyOpt);
        channel = std::move(selected);
      }
    }
  }

  return channel;
}

void ConnectionTracker::RemoveChannel(const std::shared_ptr<Endpoint>& channel) {
  auto predicate = [&channel](const auto& item) { return item.second.Channel == channel; };
  std::erase_if(_Ip4TcpTable, predicate);
  std::erase_if(_Ip6TcpTable, predicate);
  std::erase_if(_Ip4UdpTable, predicate);
  std::erase_if(_Ip6UdpTable, predicate);
  std::erase_if(_IcmpTable, predicate);
  std::erase_if(_Icmp6Table, predicate);
}

void ConnectionTracker::Prune() {
  auto now = std::chrono::steady_clock::now();
  auto predicate = [this, now](const auto& item) { return now - item.second.LastActive > _Timeout; };
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
      if (type == 8 || type == 0) {
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
          if (type == 128 || type == 129) {
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

} // namespace gh
