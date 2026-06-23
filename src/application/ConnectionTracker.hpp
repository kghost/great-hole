#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>

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

    std::strong_ordering operator<=>(const Ip4TcpKey& other) const = default;
    bool operator==(const Ip4TcpKey&) const = default;
  };

  struct Ip6TcpKey {
    boost::asio::ip::address_v6 LocalAddress;
    boost::asio::ip::address_v6 RemoteAddress;
    uint16_t LocalPort = 0;
    uint16_t RemotePort = 0;

    std::strong_ordering operator<=>(const Ip6TcpKey& other) const = default;
    bool operator==(const Ip6TcpKey&) const = default;
  };

  struct Ip4UdpKey {
    boost::asio::ip::address_v4 LocalAddress;
    boost::asio::ip::address_v4 RemoteAddress;
    uint16_t LocalPort = 0;
    uint16_t RemotePort = 0;

    std::strong_ordering operator<=>(const Ip4UdpKey& other) const = default;
    bool operator==(const Ip4UdpKey&) const = default;
  };

  struct Ip6UdpKey {
    boost::asio::ip::address_v6 LocalAddress;
    boost::asio::ip::address_v6 RemoteAddress;
    uint16_t LocalPort = 0;
    uint16_t RemotePort = 0;

    std::strong_ordering operator<=>(const Ip6UdpKey& other) const = default;
    bool operator==(const Ip6UdpKey&) const = default;
  };

  struct IcmpKey {
    boost::asio::ip::address_v4 LocalAddress;
    boost::asio::ip::address_v4 RemoteAddress;
    uint16_t Id = 0;

    std::strong_ordering operator<=>(const IcmpKey& other) const = default;
    bool operator==(const IcmpKey&) const = default;
  };

  struct Icmp6Key {
    boost::asio::ip::address_v6 LocalAddress;
    boost::asio::ip::address_v6 RemoteAddress;
    uint16_t Id = 0;

    std::strong_ordering operator<=>(const Icmp6Key& other) const = default;
    bool operator==(const Icmp6Key&) const = default;
  };

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

  using ValidatorType = std::function<bool(ConnectionMark&)>;

  explicit ConnectionTracker(boost::asio::any_io_executor executor, Selector& selector);
  ~ConnectionTracker() override = default;

  struct ConnectionEntry {
    ConnectionMark& Mark;
    std::chrono::steady_clock::time_point LastActive;
  };

  struct TcpEntry : public ConnectionEntry {
    enum class TcpState { kSynSent, kEstablished, kFinWait, kClosed };

    explicit TcpEntry(ConnectionMark& mark, std::chrono::steady_clock::time_point lastActive, uint8_t flags)
        : ConnectionEntry{mark, lastActive}, State(InitialState(flags)) {}

    TcpState State;
    static constexpr std::chrono::seconds SynTimeout = std::chrono::seconds(60);
    static constexpr std::chrono::seconds EstablishedTimeout = std::chrono::seconds(1200);
    static constexpr std::chrono::seconds FinTimeout = std::chrono::seconds(30);
    std::chrono::seconds GetTimeout() const {
      switch (State) {
      case TcpState::kSynSent:
        return SynTimeout;
      case TcpState::kEstablished:
        return EstablishedTimeout;
      case TcpState::kFinWait:
        return FinTimeout;
      case TcpState::kClosed:
        return FinTimeout;
      }
      return FinTimeout;
    }

    static TcpState InitialState(uint8_t flags) {
      if (flags & 0x04) { // RST
        return TcpEntry::TcpState::kClosed;
      } else if (flags & 0x01) { // FIN
        return TcpEntry::TcpState::kFinWait;
      } else if ((flags & 0x02) && !(flags & 0x10)) { // SYN only
        return TcpEntry::TcpState::kSynSent;
      } else if (flags & 0x10) { // ACK
        return TcpEntry::TcpState::kEstablished;
      }
      return TcpEntry::TcpState::kClosed;
    }

    void UpdateState(uint8_t flags) {
      if (flags & 0x04) { // RST
        State = TcpEntry::TcpState::kClosed;
      } else if (flags & 0x01) { // FIN
        State = TcpEntry::TcpState::kFinWait;
      } else if ((flags & 0x02) && !(flags & 0x10)) { // SYN only
        State = TcpEntry::TcpState::kSynSent;
      } else if (flags & 0x10) { // ACK
        if (State == TcpEntry::TcpState::kSynSent) {
          State = TcpEntry::TcpState::kEstablished;
        }
      }
    }
  };

  struct UdpEntry : public ConnectionEntry {
    UdpEntry(ConnectionMark& mark, std::chrono::steady_clock::time_point lastActive, int)
        : ConnectionEntry{mark, lastActive} {}
    static constexpr std::chrono::seconds Timeout = std::chrono::seconds(30);
    std::chrono::seconds GetTimeout() const { return Timeout; }
    void UpdateState(int) {}
  };

  struct IcmpConnEntry : public ConnectionEntry {
    IcmpConnEntry(ConnectionMark& mark, std::chrono::steady_clock::time_point lastActive, int)
        : ConnectionEntry{mark, lastActive} {}
    static constexpr std::chrono::seconds Timeout = std::chrono::seconds(30);
    std::chrono::seconds GetTimeout() const { return Timeout; }
    void UpdateState(int) {}
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

  template <ConnectionDirection Direction>
  std::optional<std::reference_wrapper<ConnectionMark>>
  LookupAndUpdate(const Packet& packet, std::optional<std::reference_wrapper<ConnectionMark>> mark = std::nullopt,
                  ValidatorType validator = nullptr);

  void RemoveMark(const ConnectionMark& mark);

  void Clear();

  std::string GetName() const override { return "ConnectionTracker"; }

protected:
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<void> DoWork() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  template <ConnectionDirection, typename F>
  static std::optional<std::reference_wrapper<ConnectionMark>> ParseConnectionKey(std::span<const uint8_t> p,
                                                                                  bool truncated, F&& f);

  boost::asio::any_io_executor _Executor;
  Selector& _Selector;
  std::set<Ip4TcpEntry, EntryCompare<Ip4TcpEntry>> _Ip4TcpTable;
  std::set<Ip6TcpEntry, EntryCompare<Ip6TcpEntry>> _Ip6TcpTable;
  std::set<Ip4UdpEntry, EntryCompare<Ip4UdpEntry>> _Ip4UdpTable;
  std::set<Ip6UdpEntry, EntryCompare<Ip6UdpEntry>> _Ip6UdpTable;
  std::set<IcmpEntry, EntryCompare<IcmpEntry>> _IcmpTable;
  std::set<Icmp6Entry, EntryCompare<Icmp6Entry>> _Icmp6Table;
};

} // namespace gh
