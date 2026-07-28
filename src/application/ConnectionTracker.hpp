#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <type_traits>
#include <utility>
#include <variant>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>

#include "ErrorCode.hpp"
#include "Packet.hpp"
#include "ServiceBase.hpp"

namespace gh {

class ConnectionMark {
public:
  explicit ConnectionMark() = default;
  virtual ~ConnectionMark() = default;

  ConnectionMark(const ConnectionMark&) = delete;
  auto operator=(const ConnectionMark&) -> ConnectionMark& = delete;
  ConnectionMark(ConnectionMark&&) = delete;
  auto operator=(ConnectionMark&&) -> ConnectionMark& = delete;

  [[nodiscard]] virtual auto GetDescription() const -> std::string = 0;
  [[nodiscard]] virtual auto Validate() const -> bool { return true; }
};

class ConnectionTracker : public ServiceBase {
private:
  struct TcpState;
  struct UdpState;
  struct IcmpState;

public:
  struct Ip4TcpKey {
    boost::asio::ip::address_v4 LocalAddress;
    boost::asio::ip::address_v4 RemoteAddress;
    uint16_t LocalPort = 0;
    uint16_t RemotePort = 0;

    auto operator<=>(const Ip4TcpKey& other) const -> std::strong_ordering = default;
    auto operator==(const Ip4TcpKey&) const -> bool = default;

    using State = TcpState;
  };

  struct Ip6TcpKey {
    boost::asio::ip::address_v6 LocalAddress;
    boost::asio::ip::address_v6 RemoteAddress;
    uint16_t LocalPort = 0;
    uint16_t RemotePort = 0;

    auto operator<=>(const Ip6TcpKey& other) const -> std::strong_ordering = default;
    auto operator==(const Ip6TcpKey&) const -> bool = default;

    using State = TcpState;
  };

  struct Ip4UdpKey {
    boost::asio::ip::address_v4 LocalAddress;
    boost::asio::ip::address_v4 RemoteAddress;
    uint16_t LocalPort = 0;
    uint16_t RemotePort = 0;

    auto operator<=>(const Ip4UdpKey& other) const -> std::strong_ordering = default;
    auto operator==(const Ip4UdpKey&) const -> bool = default;

    using State = UdpState;
  };

  struct Ip6UdpKey {
    boost::asio::ip::address_v6 LocalAddress;
    boost::asio::ip::address_v6 RemoteAddress;
    uint16_t LocalPort = 0;
    uint16_t RemotePort = 0;

    auto operator<=>(const Ip6UdpKey& other) const -> std::strong_ordering = default;
    auto operator==(const Ip6UdpKey&) const -> bool = default;

    using State = UdpState;
  };

  struct IcmpKey {
    boost::asio::ip::address_v4 LocalAddress;
    boost::asio::ip::address_v4 RemoteAddress;
    uint16_t Id = 0;

    auto operator<=>(const IcmpKey& other) const -> std::strong_ordering = default;
    auto operator==(const IcmpKey&) const -> bool = default;

    using State = IcmpState;
  };

  struct Icmp6Key {
    boost::asio::ip::address_v6 LocalAddress;
    boost::asio::ip::address_v6 RemoteAddress;
    uint16_t Id = 0;

    auto operator<=>(const Icmp6Key& other) const -> std::strong_ordering = default;
    auto operator==(const Icmp6Key&) const -> bool = default;

    using State = IcmpState;
  };

  using ConnectionKey = std::variant<Ip4TcpKey, Ip6TcpKey, Ip4UdpKey, Ip6UdpKey, IcmpKey, Icmp6Key>;

  // Selector determines connection routing decision and SNAT configuration:
  // - Returns a Selector::Action representing the routing decision.
  class Selector {
  public:
    struct Action {
      struct Snat4 {
        boost::asio::ip::address_v4 LocalAddress;
        std::optional<uint16_t> LocalPort;
      };

      struct Snat6 {
        boost::asio::ip::address_v6 LocalAddress;
        std::optional<uint16_t> LocalPort;
      };

      std::shared_ptr<ConnectionMark> Mark;
      std::variant<std::monostate, Snat4, Snat6> Nat;

      explicit Action(std::shared_ptr<ConnectionMark> mark) : Mark(std::move(mark)), Nat(std::monostate{}) {}
      explicit Action(std::shared_ptr<ConnectionMark> mark, Snat4 snat) : Mark(std::move(mark)), Nat(std::move(snat)) {}
      explicit Action(std::shared_ptr<ConnectionMark> mark, Snat6 snat) : Mark(std::move(mark)), Nat(std::move(snat)) {}
    };

    explicit Selector() = default;
    virtual ~Selector() = default;

    Selector(const Selector&) = delete;
    auto operator=(const Selector&) -> Selector& = delete;
    Selector(Selector&&) = delete;
    auto operator=(Selector&&) -> Selector& = delete;

    virtual auto Select(const ConnectionKey& key) -> Action = 0;
  };

  explicit ConnectionTracker(boost::asio::any_io_executor executor);
  ~ConnectionTracker() override = default;

  ConnectionTracker(const ConnectionTracker&) = delete;
  auto operator=(const ConnectionTracker&) -> ConnectionTracker& = delete;
  ConnectionTracker(ConnectionTracker&&) = delete;
  auto operator=(ConnectionTracker&&) -> ConnectionTracker& = delete;

  struct TrackedEntry {
    ConnectionKey Key;
    std::string Mark;
  };

  auto GetName() const -> std::string override { return "ConnectionTracker"; }
  void Clear();
  [[nodiscard]] auto GetConnections() const -> std::vector<TrackedEntry>;

  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

  struct Nothing {};
  struct ConnectionDirectionOutput;
  struct ConnectionDirectionInput;
  struct ConnectionDirectionOutput {
    using OppositeDirection = ConnectionDirectionInput;
  };
  struct ConnectionDirectionInput {
    using OppositeDirection = ConnectionDirectionOutput;
  };

  using Result = std::expected<std::shared_ptr<ConnectionMark>, ErrorCode>;

  template <typename Direction> auto LookupAndUpdate(Packet& packet, Selector& selector) -> Result;

private:
  struct ConnectionState {
    template <typename Self>
    auto Validate(this Self& self, std::chrono::time_point<std::chrono::steady_clock> now) -> bool {
      return !self.IsExpired(now) && self.ConnectionEntryMark->Validate();
    }

    template <typename Self>
    auto IsExpired(this Self& self, std::chrono::time_point<std::chrono::steady_clock> now) -> bool {
      return now - self.LastActive > self.GetTimeout();
    }

    std::shared_ptr<ConnectionMark> ConnectionEntryMark;
    std::chrono::steady_clock::time_point LastActive;
    static constexpr std::chrono::seconds ProneInterval = std::chrono::seconds(60);
  };

  struct TcpState : public ConnectionState {
    struct TcpFlags {
      static constexpr uint8_t kFin = 0x01;
      static constexpr uint8_t kSyn = 0x02;
      static constexpr uint8_t kRst = 0x04;
      static constexpr uint8_t kPsh = 0x08;
      static constexpr uint8_t kAck = 0x10;
      static constexpr uint8_t kUrg = 0x20;
    };

    struct TcpExtraKey {
      uint8_t Flags = 0;
      uint32_t SequenceNumber = 0;
      uint32_t AcknowledgementNumber = 0;
    };

    using ExtraKeyType = TcpExtraKey;

    struct OneDirectionState {
      enum class State : uint8_t { kNone, kSynSent, kSynAcked, kFinSent, kFinAcked, kClosed } State = State::kNone;
      uint32_t SequenceNumber = 0;
      uint32_t AcknowledgedNumber = 0;
      static constexpr std::chrono::seconds SynTimeout = std::chrono::seconds(60);
      static constexpr std::chrono::seconds EstablishedTimeout = std::chrono::seconds(1200);
      static constexpr std::chrono::seconds FinTimeout = std::chrono::seconds(30);

      [[nodiscard]] auto IsClosing() const -> bool {
        return State == State::kFinSent || State == State::kFinAcked || State == State::kClosed;
      }

      [[nodiscard]] auto IsEstablished() const -> bool { return State == State::kSynAcked; }

      auto HandleThisDirectionPacket(TcpExtraKey extra) -> bool {
        if ((extra.Flags & TcpFlags::kRst) != 0) {
          State = State::kClosed;
          return true;
        }
        switch (State) {
        case State::kNone:
          if ((extra.Flags & TcpFlags::kSyn) != 0) {
            State = State::kSynSent;
            SequenceNumber = extra.SequenceNumber;
            AcknowledgedNumber = extra.SequenceNumber;
          }
          break;
        case State::kSynSent:
          break;
        case State::kSynAcked:
          if ((extra.Flags & TcpFlags::kFin) != 0) {
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

      auto HandleOppositeDirectionPacket(TcpExtraKey extra) -> bool {
        if ((extra.Flags & TcpFlags::kRst) != 0) {
          State = State::kClosed;
          return true;
        }
        switch (State) {
        case State::kNone:
          break;
        case State::kSynSent:
          if (((extra.Flags & TcpFlags::kAck) != 0) && (extra.AcknowledgementNumber == SequenceNumber + 1)) {
            State = State::kSynAcked;
            AcknowledgedNumber = extra.AcknowledgementNumber;
          }
          break;
        case State::kSynAcked:
          break;
        case State::kFinSent:
          if (((extra.Flags & TcpFlags::kAck) != 0) && (extra.AcknowledgementNumber == SequenceNumber + 1)) {
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
    explicit TcpState(std::in_place_type_t<Direction> /*unused*/, auto&& mark,
                      std::chrono::steady_clock::time_point lastActive, TcpExtraKey extra)
        : ConnectionState{mark(), lastActive} {
      UpdateState<Direction>(extra);
    }

    [[nodiscard]] auto GetTimeout() const -> std::chrono::seconds {
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

  struct UdpState : public ConnectionState {
    template <typename Direction>
    UdpState(std::in_place_type_t<Direction> /*unused*/, auto&& mark, std::chrono::steady_clock::time_point lastActive,
             Nothing /*unused*/)
        : ConnectionState{mark(), lastActive} {}
    static constexpr std::chrono::seconds Timeout = std::chrono::seconds(30);
    static auto GetTimeout() -> std::chrono::seconds { return Timeout; }
    template <typename Direction> void UpdateState(Nothing /*unused*/) {}
    using ExtraKeyType = Nothing;
  };

  struct IcmpState : public ConnectionState {
    template <typename Direction>
    IcmpState(std::in_place_type_t<Direction> /*unused*/, auto&& mark, std::chrono::steady_clock::time_point lastActive,
              Nothing /*unused*/)
        : ConnectionState{mark(), lastActive} {}
    static constexpr std::chrono::seconds Timeout = std::chrono::seconds(30);
    static auto GetTimeout() -> std::chrono::seconds { return Timeout; }
    template <typename Direction> void UpdateState(Nothing /*unused*/) {}
    using ExtraKeyType = Nothing;
  };

  template <typename ConnectionKeyType> struct ConnectionEntry {
    using KeyType = ConnectionKeyType;
    static constexpr bool IsNat = false;
    ConnectionKeyType Key;
    typename ConnectionKeyType::State State;

    [[nodiscard]] auto GetOriginalKey() const -> const ConnectionKeyType& { return Key; }
    [[nodiscard]] auto GetReverseKey() const -> const ConnectionKeyType& { return Key; }
  };

  template <typename ConnectionKeyType> struct ConnectionNatEntry {
    using KeyType = ConnectionKeyType;
    static constexpr bool IsNat = true;
    ConnectionKeyType Key;    // Original outbound key
    ConnectionKeyType NatKey; // Translated (SNAT) key
    typename ConnectionKeyType::State State;

    [[nodiscard]] auto GetOriginalKey() const -> const ConnectionKeyType& { return Key; }
    [[nodiscard]] auto GetReverseKey() const -> const ConnectionKeyType& { return NatKey; }
  };

  using TrackedConnectionEntry =
      std::variant<ConnectionEntry<Ip4TcpKey>, ConnectionEntry<Ip6TcpKey>, ConnectionEntry<Ip4UdpKey>,
                   ConnectionEntry<Ip6UdpKey>, ConnectionEntry<IcmpKey>, ConnectionEntry<Icmp6Key>,
                   ConnectionNatEntry<Ip4TcpKey>, ConnectionNatEntry<Ip6TcpKey>, ConnectionNatEntry<Ip4UdpKey>,
                   ConnectionNatEntry<Ip6UdpKey>, ConnectionNatEntry<IcmpKey>, ConnectionNatEntry<Icmp6Key>>;

  using ConnectionEntryPtr = std::shared_ptr<TrackedConnectionEntry>;

  static auto GetOutputKey(const TrackedConnectionEntry& entry) -> ConnectionKey {
    return std::visit([](const auto& entry) -> ConnectionKey { return ConnectionKey{entry.GetOriginalKey()}; }, entry);
  }

  static auto GetInputKey(const TrackedConnectionEntry& entry) -> ConnectionKey {
    return std::visit([](const auto& entry) -> ConnectionKey { return ConnectionKey{entry.GetReverseKey()}; }, entry);
  }

  struct CompareByOutputKey {
    using is_transparent = void;

    auto operator()(const ConnectionEntryPtr& lhs, const ConnectionEntryPtr& rhs) const -> bool {
      return GetOutputKey(*lhs) < GetOutputKey(*rhs);
    }
    auto operator()(const ConnectionEntryPtr& lhs, const ConnectionKey& rhs) const -> bool {
      return GetOutputKey(*lhs) < rhs;
    }
    auto operator()(const ConnectionKey& lhs, const ConnectionEntryPtr& rhs) const -> bool {
      return lhs < GetOutputKey(*rhs);
    }
  };

  struct CompareByInputKey {
    using is_transparent = void;

    auto operator()(const ConnectionEntryPtr& lhs, const ConnectionEntryPtr& rhs) const -> bool {
      return GetInputKey(*lhs) < GetInputKey(*rhs);
    }
    auto operator()(const ConnectionEntryPtr& lhs, const ConnectionKey& rhs) const -> bool {
      return GetInputKey(*lhs) < rhs;
    }
    auto operator()(const ConnectionKey& lhs, const ConnectionEntryPtr& rhs) const -> bool {
      return lhs < GetInputKey(*rhs);
    }
  };

  enum class PacketType : std::uint8_t {
    kRealPacket,
    kIcmpInnerPacket,
  };

  template <typename KeyDirection>
  static auto ParseConnectionKey(std::span<uint8_t> packet, PacketType type, auto&& function) -> Result;

  template <typename Direction> static void ApplySnat(Packet& packet, const ConnectionKey& key);
  template <typename ConnectionKeyType, typename Nat>
  auto BuildNatKey(const ConnectionKeyType& orig, const Nat& snat) -> std::optional<ConnectionKeyType>;

  template <typename Direction, typename EntryType>
  void UpdateEntryState(EntryType& entry, const typename EntryType::KeyType::State::ExtraKeyType& extra);

  boost::asio::any_io_executor _Executor;
  std::set<ConnectionEntryPtr, CompareByOutputKey> _OutputTable;
  std::set<ConnectionEntryPtr, CompareByInputKey> _InputTable;
};

inline auto operator<<(std::ostream& stream, const ConnectionTracker::Ip4TcpKey& key) -> std::ostream& {
  return stream << key.LocalAddress.to_string() << ":" << key.LocalPort << " -> " << key.RemoteAddress.to_string()
                << ":" << key.RemotePort;
}

inline auto operator<<(std::ostream& stream, const ConnectionTracker::Ip6TcpKey& key) -> std::ostream& {
  return stream << "[" << key.LocalAddress.to_string() << "]:" << key.LocalPort << " -> ["
                << key.RemoteAddress.to_string() << "]:" << key.RemotePort;
}

inline auto operator<<(std::ostream& stream, const ConnectionTracker::Ip4UdpKey& key) -> std::ostream& {
  return stream << key.LocalAddress.to_string() << ":" << key.LocalPort << " -> " << key.RemoteAddress.to_string()
                << ":" << key.RemotePort;
}

inline auto operator<<(std::ostream& stream, const ConnectionTracker::Ip6UdpKey& key) -> std::ostream& {
  return stream << "[" << key.LocalAddress.to_string() << "]:" << key.LocalPort << " -> ["
                << key.RemoteAddress.to_string() << "]:" << key.RemotePort;
}

inline auto operator<<(std::ostream& stream, const ConnectionTracker::IcmpKey& key) -> std::ostream& {
  return stream << key.LocalAddress.to_string() << " -> " << key.RemoteAddress.to_string() << " (ID: " << key.Id << ")";
}

inline auto operator<<(std::ostream& stream, const ConnectionTracker::Icmp6Key& key) -> std::ostream& {
  return stream << "[" << key.LocalAddress.to_string() << "] -> [" << key.RemoteAddress.to_string()
                << "] (ID: " << key.Id << ")";
}

inline auto operator<<(std::ostream& stream, const ConnectionTracker::ConnectionKey& key) -> std::ostream& {
  std::visit([&stream](const auto& key) -> auto { stream << key; }, key);
  return stream;
}

} // namespace gh
