#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <set>
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

  enum class TcpState { kSynSent, kEstablished, kFinWait, kClosed };

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

  explicit ConnectionTracker(boost::asio::any_io_executor executor, SelectorType selector);
  ~ConnectionTracker() override = default;

  struct ConnectionEntry {
    ConnectionMark& Mark;
    mutable std::chrono::steady_clock::time_point LastActive;
  };

  struct TcpEntry : public ConnectionEntry {
    TcpEntry(ConnectionMark& mark, std::chrono::steady_clock::time_point lastActive,
             TcpState state = TcpState::kSynSent)
        : ConnectionEntry{mark, lastActive}, State(state) {}

    mutable TcpState State = TcpState::kSynSent;
    static std::chrono::seconds SynTimeout;
    static std::chrono::seconds EstablishedTimeout;
    static std::chrono::seconds FinTimeout;
  };

  struct UdpEntry : public ConnectionEntry {
    UdpEntry(ConnectionMark& mark, std::chrono::steady_clock::time_point lastActive)
        : ConnectionEntry{mark, lastActive} {}
    static std::chrono::seconds Timeout;
  };

  struct IcmpConnEntry : public ConnectionEntry {
    IcmpConnEntry(ConnectionMark& mark, std::chrono::steady_clock::time_point lastActive)
        : ConnectionEntry{mark, lastActive} {}
    static std::chrono::seconds Timeout;
  };

  struct Ip4TcpEntry {
    Ip4TcpKey Key;
    mutable TcpEntry Entry;
  };

  struct Ip6TcpEntry {
    Ip6TcpKey Key;
    mutable TcpEntry Entry;
  };

  struct Ip4UdpEntry {
    Ip4UdpKey Key;
    mutable UdpEntry Entry;
  };

  struct Ip6UdpEntry {
    Ip6UdpKey Key;
    mutable UdpEntry Entry;
  };

  struct IcmpEntry {
    IcmpKey Key;
    mutable IcmpConnEntry Entry;
  };

  struct Icmp6Entry {
    Icmp6Key Key;
    mutable IcmpConnEntry Entry;
  };

  template <typename EntryType> struct EntryCompare {
    using is_transparent = void;
    using KeyType = std::decay_t<decltype(std::declval<EntryType>().Key)>;

    bool operator()(const EntryType& lhs, const EntryType& rhs) const { return lhs.Key < rhs.Key; }
    bool operator()(const EntryType& lhs, const KeyType& rhs) const { return lhs.Key < rhs; }
    bool operator()(const KeyType& lhs, const EntryType& rhs) const { return lhs < rhs.Key; }
  };

  ConnectionTracker(const ConnectionTracker&) = delete;
  ConnectionTracker& operator=(const ConnectionTracker&) = delete;
  ConnectionTracker(ConnectionTracker&&) = delete;
  ConnectionTracker& operator=(ConnectionTracker&&) = delete;

  void Update(const Packet& packet, ConnectionMark& mark, ConnectionDirection direction);

  std::optional<std::reference_wrapper<ConnectionMark>> Lookup(const Packet& packet, ConnectionDirection direction,
                                                               ValidatorType validator = nullptr);

  void RemoveMark(const ConnectionMark& mark);

  void Clear();

  std::string GetName() const override { return "ConnectionTracker"; }

protected:
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<void> DoWork() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  static std::optional<ConnectionKey> ParseConnectionKey(const Packet& p, ConnectionDirection direction);
  static std::optional<uint8_t> GetTcpFlags(const Packet& p);

  static void UpdateTcpState(TcpEntry& entry, uint8_t flags) {
    if (flags & 0x04) { // RST
      entry.State = TcpState::kClosed;
    } else if (flags & 0x01) { // FIN
      entry.State = TcpState::kFinWait;
    } else if ((flags & 0x02) && !(flags & 0x10)) { // SYN only
      entry.State = TcpState::kSynSent;
    } else if (flags & 0x10) { // ACK
      if (entry.State == TcpState::kSynSent) {
        entry.State = TcpState::kEstablished;
      }
    }
  }

  boost::asio::any_io_executor _Executor;
  SelectorType _Selector;
  std::set<Ip4TcpEntry, EntryCompare<Ip4TcpEntry>> _Ip4TcpTable;
  std::set<Ip6TcpEntry, EntryCompare<Ip6TcpEntry>> _Ip6TcpTable;
  std::set<Ip4UdpEntry, EntryCompare<Ip4UdpEntry>> _Ip4UdpTable;
  std::set<Ip6UdpEntry, EntryCompare<Ip6UdpEntry>> _Ip6UdpTable;
  std::set<IcmpEntry, EntryCompare<IcmpEntry>> _IcmpTable;
  std::set<Icmp6Entry, EntryCompare<Icmp6Entry>> _Icmp6Table;
};

} // namespace gh
