#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <utility>

#include <boost/asio/io_context.hpp>
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

  [[nodiscard]] virtual auto ToPacketMark() const -> std::unique_ptr<PacketMark> { return nullptr; }
};

class ConnectionTracker : public ServiceBase {
public:
  struct Ip4TcpKey {
    boost::asio::ip::address_v4 LocalAddress;
    boost::asio::ip::address_v4 RemoteAddress;
    uint16_t LocalPort = 0;
    uint16_t RemotePort = 0;

    auto operator<=>(const Ip4TcpKey& other) const -> std::strong_ordering = default;
    auto operator==(const Ip4TcpKey&) const -> bool = default;
  };

  struct Ip6TcpKey {
    boost::asio::ip::address_v6 LocalAddress;
    boost::asio::ip::address_v6 RemoteAddress;
    uint16_t LocalPort = 0;
    uint16_t RemotePort = 0;

    auto operator<=>(const Ip6TcpKey& other) const -> std::strong_ordering = default;
    auto operator==(const Ip6TcpKey&) const -> bool = default;
  };

  struct Ip4UdpKey {
    boost::asio::ip::address_v4 LocalAddress;
    boost::asio::ip::address_v4 RemoteAddress;
    uint16_t LocalPort = 0;
    uint16_t RemotePort = 0;

    auto operator<=>(const Ip4UdpKey& other) const -> std::strong_ordering = default;
    auto operator==(const Ip4UdpKey&) const -> bool = default;
  };

  struct Ip6UdpKey {
    boost::asio::ip::address_v6 LocalAddress;
    boost::asio::ip::address_v6 RemoteAddress;
    uint16_t LocalPort = 0;
    uint16_t RemotePort = 0;

    auto operator<=>(const Ip6UdpKey& other) const -> std::strong_ordering = default;
    auto operator==(const Ip6UdpKey&) const -> bool = default;
  };

  struct IcmpKey {
    boost::asio::ip::address_v4 LocalAddress;
    boost::asio::ip::address_v4 RemoteAddress;
    uint16_t Id = 0;

    auto operator<=>(const IcmpKey& other) const -> std::strong_ordering = default;
    auto operator==(const IcmpKey&) const -> bool = default;
  };

  struct Icmp6Key {
    boost::asio::ip::address_v6 LocalAddress;
    boost::asio::ip::address_v6 RemoteAddress;
    uint16_t Id = 0;

    auto operator<=>(const Icmp6Key& other) const -> std::strong_ordering = default;
    auto operator==(const Icmp6Key&) const -> bool = default;
  };

  // Selector determines connection routing:
  // - Returns a std::unique_ptr<ConnectionMark> representing the routing decision.
  //
  // Explicit Guarantee:
  // The Selector implementation must guarantee loop prevention by returning a 'Bypass'
  // mark for all traffic destined for the active VPN server endpoints.
  class Selector {
  public:
    explicit Selector() = default;
    virtual ~Selector() = default;

    Selector(const Selector&) = delete;
    auto operator=(const Selector&) -> Selector& = delete;
    Selector(Selector&&) = delete;
    auto operator=(Selector&&) -> Selector& = delete;

    virtual auto operator()(const Ip4TcpKey&) const -> std::unique_ptr<ConnectionMark> = 0;
    virtual auto operator()(const Ip6TcpKey&) const -> std::unique_ptr<ConnectionMark> = 0;
    virtual auto operator()(const Ip4UdpKey&) const -> std::unique_ptr<ConnectionMark> = 0;
    virtual auto operator()(const Ip6UdpKey&) const -> std::unique_ptr<ConnectionMark> = 0;
    virtual auto operator()(const IcmpKey&) const -> std::unique_ptr<ConnectionMark> = 0;
    virtual auto operator()(const Icmp6Key&) const -> std::unique_ptr<ConnectionMark> = 0;
  };

  explicit ConnectionTracker(boost::asio::any_io_executor executor);
  ~ConnectionTracker() override = default;

  ConnectionTracker(const ConnectionTracker&) = delete;
  auto operator=(const ConnectionTracker&) -> ConnectionTracker& = delete;
  ConnectionTracker(ConnectionTracker&&) = delete;
  auto operator=(ConnectionTracker&&) -> ConnectionTracker& = delete;

  auto GetName() const -> std::string override { return "ConnectionTracker"; }
  void Clear();

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

  using Result = std::expected<std::reference_wrapper<ConnectionMark>, ErrorCode>;

  template <typename Direction> auto LookupAndUpdate(const Packet& packet, Selector& selector) -> Result;

private:
  struct ConnectionEntry {
    template <typename Self>
    auto Validate(this Self& self, std::chrono::time_point<std::chrono::steady_clock> now) -> bool {
      return !self.IsExpired(now) && self.ConnectionMark->Validate();
    }

    template <typename Self>
    auto IsExpired(this Self& self, std::chrono::time_point<std::chrono::steady_clock> now) -> bool {
      return now - self.LastActive > self.GetTimeout();
    }

    std::unique_ptr<ConnectionMark> ConnectionMark;
    std::chrono::steady_clock::time_point LastActive;
    static constexpr std::chrono::seconds ProneInterval = std::chrono::seconds(60);
  };

  struct TcpEntry : public ConnectionEntry {
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
    explicit TcpEntry(std::in_place_type_t<Direction> /*unused*/, auto&& mark,
                      std::chrono::steady_clock::time_point lastActive, TcpExtraKey extra)
        : ConnectionEntry{mark(), lastActive} {
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

  struct UdpEntry : public ConnectionEntry {
    template <typename Direction>
    UdpEntry(std::in_place_type_t<Direction> /*unused*/, auto&& mark, std::chrono::steady_clock::time_point lastActive,
             Nothing /*unused*/)
        : ConnectionEntry{mark(), lastActive} {}
    static constexpr std::chrono::seconds Timeout = std::chrono::seconds(30);
    static auto GetTimeout() -> std::chrono::seconds { return Timeout; }
    template <typename Direction> void UpdateState(Nothing /*unused*/) {}
  };

  struct IcmpConnEntry : public ConnectionEntry {
    template <typename Direction>
    IcmpConnEntry(std::in_place_type_t<Direction> /*unused*/, auto&& mark,
                  std::chrono::steady_clock::time_point lastActive, Nothing /*unused*/)
        : ConnectionEntry{mark(), lastActive} {}
    static constexpr std::chrono::seconds Timeout = std::chrono::seconds(30);
    static auto GetTimeout() -> std::chrono::seconds { return Timeout; }
    template <typename Direction> void UpdateState(Nothing /*unused*/) {}
  };

  enum class PacketType : std::uint8_t {
    kRealPacket,
    kIcmpInnerPacket,
  };

  template <typename KeyDirection>
  static auto ParseConnectionKey(std::span<const uint8_t> packet, PacketType type, auto&& function) -> Result;

  boost::asio::any_io_executor _Executor;
  std::map<Ip4TcpKey, TcpEntry> _Ip4TcpTable;
  std::map<Ip6TcpKey, TcpEntry> _Ip6TcpTable;
  std::map<Ip4UdpKey, UdpEntry> _Ip4UdpTable;
  std::map<Ip6UdpKey, UdpEntry> _Ip6UdpTable;
  std::map<IcmpKey, IcmpConnEntry> _IcmpTable;
  std::map<Icmp6Key, IcmpConnEntry> _Icmp6Table;
};

} // namespace gh
