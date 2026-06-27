#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <expected>
#include <functional>
#include <set>
#include <variant>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>

#include "Packet.hpp"
#include "ServiceBase.hpp"

namespace gh {

class ConnectionMark {
public:
  virtual ~ConnectionMark() = default;
  virtual std::string GetDescription() const = 0;
};

class ConnectionTracker : public ServiceBase {
public:
  struct UnsupportedPacket {};

  struct ToBeSelected {};
  struct Bypass {};
  struct Discard {};
  using RouteResult = std::variant<ToBeSelected, Bypass, Discard, std::reference_wrapper<ConnectionMark>>;

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

  // Selector determines connection routing:
  // - Bypass: Packets should bypass VPN (e.g. packets destined for the active VPN servers).
  // - Discard: Packets should be dropped.
  // - std::reference_wrapper<ConnectionMark>: Route via the chosen session.
  //
  // Explicit Guarantee:
  // The Selector implementation must guarantee loop prevention by returning 'Bypass'
  // for all traffic destined for the active VPN server endpoints.
  class Selector {
  public:
    virtual ~Selector() = default;
    virtual RouteResult operator()(const Ip4TcpKey&) const = 0;
    virtual RouteResult operator()(const Ip6TcpKey&) const = 0;
    virtual RouteResult operator()(const Ip4UdpKey&) const = 0;
    virtual RouteResult operator()(const Ip6UdpKey&) const = 0;
    virtual RouteResult operator()(const IcmpKey&) const = 0;
    virtual RouteResult operator()(const Icmp6Key&) const = 0;
  };

  using ValidatorType = std::function<bool(ConnectionMark&)>;

  explicit ConnectionTracker(boost::asio::any_io_executor executor, Selector& selector);
  ~ConnectionTracker() override = default;

  ConnectionTracker(const ConnectionTracker&) = delete;
  ConnectionTracker& operator=(const ConnectionTracker&) = delete;
  ConnectionTracker(ConnectionTracker&&) = delete;
  ConnectionTracker& operator=(ConnectionTracker&&) = delete;

  std::string GetName() const override { return "ConnectionTracker"; }
  void RemoveMark(const ConnectionMark& mark);
  void Clear();

  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<void> DoWork() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

  struct Nothing {};
  struct ConnectionDirectionOutput;
  struct ConnectionDirectionInput;
  struct ConnectionDirectionOutput {
    using OppositeDirection = ConnectionDirectionInput;
    using ConnectionTrackerInput = Nothing;
    using ConnectionTrackerOutput = RouteResult;
  };
  struct ConnectionDirectionInput {
    using OppositeDirection = ConnectionDirectionOutput;
    using ConnectionTrackerInput = ConnectionMark&;
    using ConnectionTrackerOutput = Nothing;
  };

  template <typename Direction>
  typename Direction::ConnectionTrackerOutput
  LookupAndUpdate(const Packet& packet, Direction::ConnectionTrackerInput input, ValidatorType validator);

private:
  struct ConnectionEntry {
    RouteResult Result;
    std::chrono::steady_clock::time_point LastActive;
    static constexpr std::chrono::seconds ProneInterval = std::chrono::seconds(60);
  };

  struct TcpEntry : public ConnectionEntry {
    enum TcpFlags : uint8_t {
      kFin = 0x01,
      kSyn = 0x02,
      kRst = 0x04,
      kPsh = 0x08,
      kAck = 0x10,
      kUrg = 0x20,
    };

    struct TcpExtraKey {
      uint8_t Flags = 0;
      uint32_t SequenceNumber = 0;
      uint32_t AcknowledgementNumber = 0;
    };

    struct OneDirectionState {
      enum class State : uint8_t { kNone, kSynSent, kSynAcked, kFinSent, kFinAcked, kClosed } State = State::kNone;
      uint32_t SequenceNumber = 0;
      uint32_t AcknowledgedNumber = 0;
      static constexpr std::chrono::seconds SynTimeout = std::chrono::seconds(60);
      static constexpr std::chrono::seconds EstablishedTimeout = std::chrono::seconds(1200);
      static constexpr std::chrono::seconds FinTimeout = std::chrono::seconds(30);

      bool IsClosing() const {
        return State == State::kFinSent || State == State::kFinAcked || State == State::kClosed;
      }

      bool IsEstablished() const { return State == State::kSynAcked; }

      bool HandleThisDirectionPacket(TcpExtraKey extra) {
        if (extra.Flags & TcpFlags::kRst) {
          State = State::kClosed;
          return true;
        }
        switch (State) {
        case State::kNone:
          if (extra.Flags & TcpFlags::kSyn) {
            State = State::kSynSent;
            SequenceNumber = extra.SequenceNumber;
            AcknowledgedNumber = extra.SequenceNumber;
          }
          break;
        case State::kSynSent:
          break;
        case State::kSynAcked:
          if (extra.Flags & TcpFlags::kFin) {
            State = State::kFinSent;
            SequenceNumber = extra.SequenceNumber;
          }
          break;
        case State::kFinSent:
        case State::kFinAcked:
        case State::kClosed:
        default:
          break;
        }
        return true;
      }

      bool HandleOppositeDirectionPacket(TcpExtraKey extra) {
        if (extra.Flags & TcpFlags::kRst) {
          State = State::kClosed;
          return true;
        }
        switch (State) {
        case State::kNone:
          break;
        case State::kSynSent:
          if ((extra.Flags & TcpFlags::kAck) && (extra.AcknowledgementNumber == SequenceNumber + 1)) {
            State = State::kSynAcked;
            AcknowledgedNumber = extra.AcknowledgementNumber;
          }
          break;
        case State::kSynAcked:
          break;
        case State::kFinSent:
          if ((extra.Flags & TcpFlags::kAck) && (extra.AcknowledgementNumber == SequenceNumber + 1)) {
            State = State::kFinAcked;
            AcknowledgedNumber = extra.AcknowledgementNumber;
          }
          break;
        case State::kFinAcked:
        case State::kClosed:
        default:
          break;
        }
        return true;
      }
    } OutputDirection, InputDirection;

    template <typename Direction>
    explicit TcpEntry(std::in_place_type_t<Direction>, RouteResult result,
                      std::chrono::steady_clock::time_point lastActive, TcpExtraKey extra)
        : ConnectionEntry{result, lastActive} {
      UpdateState<Direction>(extra);
    }

    std::chrono::seconds GetTimeout() const {
      if (OutputDirection.IsClosing() || InputDirection.IsClosing()) {
        return OneDirectionState::FinTimeout;
      }
      if (OutputDirection.IsEstablished() || InputDirection.IsEstablished()) {
        return OneDirectionState::EstablishedTimeout;
      }
      return OneDirectionState::SynTimeout;
    }

    template <typename Direction> void UpdateState(TcpExtraKey extra) {
      if constexpr (std::is_same_v<Direction, ConnectionDirectionOutput>) {
        OutputDirection.HandleThisDirectionPacket(extra);
        InputDirection.HandleOppositeDirectionPacket(extra);
      } else {
        InputDirection.HandleThisDirectionPacket(extra);
        OutputDirection.HandleOppositeDirectionPacket(extra);
      }
    }
  };

  struct UdpEntry : public ConnectionEntry {
    template <typename Direction>
    UdpEntry(std::in_place_type_t<Direction>, RouteResult result, std::chrono::steady_clock::time_point lastActive,
             Nothing)
        : ConnectionEntry{result, lastActive} {}
    static constexpr std::chrono::seconds Timeout = std::chrono::seconds(30);
    std::chrono::seconds GetTimeout() const { return Timeout; }
    template <typename Direction> void UpdateState(Nothing) {}
  };

  struct IcmpConnEntry : public ConnectionEntry {
    template <typename Direction>
    IcmpConnEntry(std::in_place_type_t<Direction>, RouteResult result, std::chrono::steady_clock::time_point lastActive,
                  Nothing)
        : ConnectionEntry{result, lastActive} {}
    static constexpr std::chrono::seconds Timeout = std::chrono::seconds(30);
    std::chrono::seconds GetTimeout() const { return Timeout; }
    template <typename Direction> void UpdateState(Nothing) {}
  };

  template <typename Entry>
  static bool IsExpired(Entry& entry, std::chrono::time_point<std::chrono::steady_clock> now) {
    return now - entry.LastActive > entry.GetTimeout();
  }

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

  enum class PacketType {
    kRealPacket,
    kIcmpInnerPacket,
  };

  template <typename KeyDirection, typename Result, typename F>
  static std::expected<Result, UnsupportedPacket> ParseConnectionKey(std::span<const uint8_t> p, PacketType type,
                                                                     F&& f);

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
