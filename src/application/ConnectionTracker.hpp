#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <variant>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>

#include "Packet.hpp"
#include "ServiceBase.hpp"

namespace gh {

enum class ConnectionDirection { kOutput, kInput };

class ConnectionMark {
public:
  virtual ~ConnectionMark() = default;
  virtual std::string GetDescription() const = 0;
};

class ConnectionTracker : public ServiceBase {
public:
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

  class Selector {
  public:
    virtual ~Selector() = default;
    virtual std::optional<std::reference_wrapper<ConnectionMark>> operator()(const Ip4TcpKey&) const = 0;
    virtual std::optional<std::reference_wrapper<ConnectionMark>> operator()(const Ip6TcpKey&) const = 0;
    virtual std::optional<std::reference_wrapper<ConnectionMark>> operator()(const Ip4UdpKey&) const = 0;
    virtual std::optional<std::reference_wrapper<ConnectionMark>> operator()(const Ip6UdpKey&) const = 0;
    virtual std::optional<std::reference_wrapper<ConnectionMark>> operator()(const IcmpKey&) const = 0;
    virtual std::optional<std::reference_wrapper<ConnectionMark>> operator()(const Icmp6Key&) const = 0;
  };

  using SelectorType = Selector&;

  using ValidatorType = std::function<bool(ConnectionMark&)>;

  explicit ConnectionTracker(boost::asio::io_context& ioContext, SelectorType selector,
                             std::chrono::seconds timeout = std::chrono::seconds(60));
  ~ConnectionTracker() override = default;

  ConnectionTracker(const ConnectionTracker&) = delete;
  ConnectionTracker& operator=(const ConnectionTracker&) = delete;
  ConnectionTracker(ConnectionTracker&&) = delete;
  ConnectionTracker& operator=(ConnectionTracker&&) = delete;

  void Update(const Packet& packet, ConnectionMark& mark, ConnectionDirection direction);

  std::optional<std::reference_wrapper<ConnectionMark>> Lookup(const Packet& packet, ConnectionDirection direction,
                                                               ValidatorType validator = nullptr);

  void RemoveMark(const ConnectionMark& mark);

  void Clear();

  void SetTimeout(std::chrono::seconds timeout) { _Timeout = timeout; }
  std::chrono::seconds GetTimeout() const { return _Timeout; }

  std::string GetName() const override { return "ConnectionTracker"; }

protected:
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<void> DoWork() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  static std::optional<ConnectionKey> ParseConnectionKey(const Packet& p, ConnectionDirection direction);

  struct ConnectionEntry {
    ConnectionMark& Mark;
    std::chrono::steady_clock::time_point LastActive;
  };

  boost::asio::io_context& _IoContext;
  SelectorType _Selector;
  std::map<Ip4TcpKey, ConnectionEntry> _Ip4TcpTable;
  std::map<Ip6TcpKey, ConnectionEntry> _Ip6TcpTable;
  std::map<Ip4UdpKey, ConnectionEntry> _Ip4UdpTable;
  std::map<Ip6UdpKey, ConnectionEntry> _Ip6UdpTable;
  std::map<IcmpKey, ConnectionEntry> _IcmpTable;
  std::map<Icmp6Key, ConnectionEntry> _Icmp6Table;
  std::chrono::seconds _Timeout;
};

} // namespace gh
