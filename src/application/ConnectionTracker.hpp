#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <variant>

#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>

#include "Endpoint.hpp"
#include "Packet.hpp"

namespace gh {

enum class ConnectionDirection { kOutput, kInput };

class ConnectionTracker {
public:
  using SelectorType = std::function<std::shared_ptr<Endpoint>(const boost::asio::ip::address_v6& src,
                                                               const boost::asio::ip::address_v6& dst, uint16_t srcPort,
                                                               uint16_t dstPort, uint8_t protocol)>;

  using ValidatorType = std::function<bool(const std::shared_ptr<Endpoint>&)>;

  explicit ConnectionTracker(std::chrono::seconds timeout = std::chrono::seconds(60));
  ~ConnectionTracker() = default;

  ConnectionTracker(const ConnectionTracker&) = delete;
  ConnectionTracker& operator=(const ConnectionTracker&) = delete;
  ConnectionTracker(ConnectionTracker&&) = delete;
  ConnectionTracker& operator=(ConnectionTracker&&) = delete;

  void Update(const Packet& packet, std::shared_ptr<Endpoint> channel, ConnectionDirection direction);

  std::shared_ptr<Endpoint> Lookup(const Packet& packet, ConnectionDirection direction,
                                   ValidatorType validator = nullptr, SelectorType selector = nullptr);

  void RemoveChannel(const std::shared_ptr<Endpoint>& channel);

  void Prune();

  void Clear();

  void SetTimeout(std::chrono::seconds timeout) { _Timeout = timeout; }
  std::chrono::seconds GetTimeout() const { return _Timeout; }

private:
  struct Ip4TcpKey {
    boost::asio::ip::address_v4 LocalAddress;
    boost::asio::ip::address_v4 RemoteAddress;
    uint16_t LocalPort = 0;
    uint16_t RemotePort = 0;

    bool operator==(const Ip4TcpKey& other) const {
      return LocalAddress == other.LocalAddress && RemoteAddress == other.RemoteAddress &&
             LocalPort == other.LocalPort && RemotePort == other.RemotePort;
    }

    bool operator<(const Ip4TcpKey& other) const {
      if (LocalAddress != other.LocalAddress) {
        return LocalAddress < other.LocalAddress;
      }
      if (RemoteAddress != other.RemoteAddress) {
        return RemoteAddress < other.RemoteAddress;
      }
      if (LocalPort != other.LocalPort) {
        return LocalPort < other.LocalPort;
      }
      return RemotePort < other.RemotePort;
    }
  };

  struct Ip6TcpKey {
    boost::asio::ip::address_v6 LocalAddress;
    boost::asio::ip::address_v6 RemoteAddress;
    uint16_t LocalPort = 0;
    uint16_t RemotePort = 0;

    bool operator==(const Ip6TcpKey& other) const {
      return LocalAddress == other.LocalAddress && RemoteAddress == other.RemoteAddress &&
             LocalPort == other.LocalPort && RemotePort == other.RemotePort;
    }

    bool operator<(const Ip6TcpKey& other) const {
      if (LocalAddress != other.LocalAddress) {
        return LocalAddress < other.LocalAddress;
      }
      if (RemoteAddress != other.RemoteAddress) {
        return RemoteAddress < other.RemoteAddress;
      }
      if (LocalPort != other.LocalPort) {
        return LocalPort < other.LocalPort;
      }
      return RemotePort < other.RemotePort;
    }
  };

  struct Ip4UdpKey {
    boost::asio::ip::address_v4 LocalAddress;
    boost::asio::ip::address_v4 RemoteAddress;
    uint16_t LocalPort = 0;
    uint16_t RemotePort = 0;

    bool operator==(const Ip4UdpKey& other) const {
      return LocalAddress == other.LocalAddress && RemoteAddress == other.RemoteAddress &&
             LocalPort == other.LocalPort && RemotePort == other.RemotePort;
    }

    bool operator<(const Ip4UdpKey& other) const {
      if (LocalAddress != other.LocalAddress) {
        return LocalAddress < other.LocalAddress;
      }
      if (RemoteAddress != other.RemoteAddress) {
        return RemoteAddress < other.RemoteAddress;
      }
      if (LocalPort != other.LocalPort) {
        return LocalPort < other.LocalPort;
      }
      return RemotePort < other.RemotePort;
    }
  };

  struct Ip6UdpKey {
    boost::asio::ip::address_v6 LocalAddress;
    boost::asio::ip::address_v6 RemoteAddress;
    uint16_t LocalPort = 0;
    uint16_t RemotePort = 0;

    bool operator==(const Ip6UdpKey& other) const {
      return LocalAddress == other.LocalAddress && RemoteAddress == other.RemoteAddress &&
             LocalPort == other.LocalPort && RemotePort == other.RemotePort;
    }

    bool operator<(const Ip6UdpKey& other) const {
      if (LocalAddress != other.LocalAddress) {
        return LocalAddress < other.LocalAddress;
      }
      if (RemoteAddress != other.RemoteAddress) {
        return RemoteAddress < other.RemoteAddress;
      }
      if (LocalPort != other.LocalPort) {
        return LocalPort < other.LocalPort;
      }
      return RemotePort < other.RemotePort;
    }
  };

  struct IcmpKey {
    boost::asio::ip::address_v4 LocalAddress;
    boost::asio::ip::address_v4 RemoteAddress;
    uint16_t Id = 0;

    bool operator==(const IcmpKey& other) const {
      return LocalAddress == other.LocalAddress && RemoteAddress == other.RemoteAddress && Id == other.Id;
    }

    bool operator<(const IcmpKey& other) const {
      if (LocalAddress != other.LocalAddress) {
        return LocalAddress < other.LocalAddress;
      }
      if (RemoteAddress != other.RemoteAddress) {
        return RemoteAddress < other.RemoteAddress;
      }
      return Id < other.Id;
    }
  };

  struct Icmp6Key {
    boost::asio::ip::address_v6 LocalAddress;
    boost::asio::ip::address_v6 RemoteAddress;
    uint16_t Id = 0;

    bool operator==(const Icmp6Key& other) const {
      return LocalAddress == other.LocalAddress && RemoteAddress == other.RemoteAddress && Id == other.Id;
    }

    bool operator<(const Icmp6Key& other) const {
      if (LocalAddress != other.LocalAddress) {
        return LocalAddress < other.LocalAddress;
      }
      if (RemoteAddress != other.RemoteAddress) {
        return RemoteAddress < other.RemoteAddress;
      }
      return Id < other.Id;
    }
  };

  using ConnectionKey = std::variant<Ip4TcpKey, Ip6TcpKey, Ip4UdpKey, Ip6UdpKey, IcmpKey, Icmp6Key>;

  static std::optional<ConnectionKey> ParseConnectionKey(const Packet& p, ConnectionDirection direction);

  struct ConnectionEntry {
    std::shared_ptr<Endpoint> Channel;
    std::chrono::steady_clock::time_point LastActive;
  };

  std::map<Ip4TcpKey, ConnectionEntry> _Ip4TcpTable;
  std::map<Ip6TcpKey, ConnectionEntry> _Ip6TcpTable;
  std::map<Ip4UdpKey, ConnectionEntry> _Ip4UdpTable;
  std::map<Ip6UdpKey, ConnectionEntry> _Ip6UdpTable;
  std::map<IcmpKey, ConnectionEntry> _IcmpTable;
  std::map<Icmp6Key, ConnectionEntry> _Icmp6Table;
  std::chrono::seconds _Timeout;
};

} // namespace gh
