#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/windows/object_handle.hpp>
#include <optional>

#include <variant>
#include <windows.h>

#include "Logger.hpp"
#include "ServiceBase.hpp"
#include "Utils/Overload.hpp"

namespace gh {

inline auto operator<=>(const boost::asio::ip::address_v4& lhs, const boost::asio::ip::address_v4& rhs) noexcept
    -> std::strong_ordering {
  return lhs.to_uint() <=> rhs.to_uint();
}

inline auto operator<=>(const boost::asio::ip::address_v6& lhs, const boost::asio::ip::address_v6& rhs) noexcept
    -> std::strong_ordering {
  return lhs.to_bytes() <=> rhs.to_bytes();
}

class WinDivertFlowSnifferCallback {
public:
  explicit WinDivertFlowSnifferCallback() = default;
  virtual ~WinDivertFlowSnifferCallback() = default;

  WinDivertFlowSnifferCallback(const WinDivertFlowSnifferCallback&) = delete;
  auto operator=(const WinDivertFlowSnifferCallback&) -> WinDivertFlowSnifferCallback& = delete;
  WinDivertFlowSnifferCallback(WinDivertFlowSnifferCallback&&) = delete;
  auto operator=(WinDivertFlowSnifferCallback&&) -> WinDivertFlowSnifferCallback& = delete;

  enum class Protocol : uint8_t { Ipv4Tcp, Ipv4Udp, Ipv6Tcp, Ipv6Udp };

  static auto ProtocolToString(WinDivertFlowSnifferCallback::Protocol proto) -> std::string {
    switch (proto) {
    case Protocol::Ipv4Tcp:
      return "TCPv4";
    case Protocol::Ipv4Udp:
      return "UDPv4";
    case Protocol::Ipv6Tcp:
      return "TCPv6";
    case Protocol::Ipv6Udp:
      return "UDPv6";
    default:
      return "Unknown";
    }
  }

  struct FlowIp4Key {
    Protocol Proto{};
    boost::asio::ip::address_v4 LocalAddress;
    uint16_t LocalPort{0};

    friend auto operator<=>(const FlowIp4Key& lhs, const FlowIp4Key& rhs) noexcept {
      if (auto cmp = lhs.Proto <=> rhs.Proto; cmp != 0) {
        return cmp;
      }
      if (auto cmp = lhs.LocalAddress.to_uint() <=> rhs.LocalAddress.to_uint(); cmp != 0) {
        return cmp;
      }
      return lhs.LocalPort <=> rhs.LocalPort;
    }
    friend bool operator==(const FlowIp4Key& lhs, const FlowIp4Key& rhs) noexcept {
      return lhs.Proto == rhs.Proto && lhs.LocalAddress == rhs.LocalAddress && lhs.LocalPort == rhs.LocalPort;
    }
    friend bool operator<(const FlowIp4Key& lhs, const FlowIp4Key& rhs) noexcept { return (lhs <=> rhs) < 0; }
  };

  struct FlowIp6Key {
    Protocol Proto{};
    boost::asio::ip::address_v6 LocalAddress;
    uint16_t LocalPort{0};

    friend auto operator<=>(const FlowIp6Key& lhs, const FlowIp6Key& rhs) noexcept {
      if (auto cmp = lhs.Proto <=> rhs.Proto; cmp != 0) {
        return cmp;
      }
      if (auto cmp = lhs.LocalAddress.to_bytes() <=> rhs.LocalAddress.to_bytes(); cmp != 0) {
        return cmp;
      }
      return lhs.LocalPort <=> rhs.LocalPort;
    }
    friend bool operator==(const FlowIp6Key& lhs, const FlowIp6Key& rhs) noexcept {
      return lhs.Proto == rhs.Proto && lhs.LocalAddress == rhs.LocalAddress && lhs.LocalPort == rhs.LocalPort;
    }
    friend bool operator<(const FlowIp6Key& lhs, const FlowIp6Key& rhs) noexcept { return (lhs <=> rhs) < 0; }
  };

  using FlowKey = std::variant<FlowIp4Key, FlowIp6Key>;

  virtual auto OnFlowEstablished(const FlowKey& key, uint32_t pid) -> Omni::Fiber::Coroutine<void> = 0;
  virtual auto OnFlowDeleted(const FlowKey& key) -> Omni::Fiber::Coroutine<void> = 0;
};

class WinDivertFlowSniffer : public ServiceBase {
public:
  explicit WinDivertFlowSniffer(boost::asio::any_io_executor executor, WinDivertFlowSnifferCallback& callback);
  ~WinDivertFlowSniffer() override;

  WinDivertFlowSniffer(const WinDivertFlowSniffer&) = delete;
  auto operator=(const WinDivertFlowSniffer&) -> WinDivertFlowSniffer& = delete;
  WinDivertFlowSniffer(WinDivertFlowSniffer&&) = delete;
  auto operator=(WinDivertFlowSniffer&&) -> WinDivertFlowSniffer& = delete;

  auto GetName() const -> std::string override { return "WinDivertFlowSniffer"; }

protected:
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  boost::asio::any_io_executor _Executor;
  WinDivertFlowSnifferCallback& _Callback;
  HANDLE _WinDivertFlowHandle = INVALID_HANDLE_VALUE;
  HANDLE _ReadEvent = nullptr;
  std::optional<boost::asio::windows::object_handle> _ReadObject;
  OVERLAPPED _Overlapped{};
  gh::base::ComponentLogger _Logger{boost::log::keywords::channel = "WinDivertFlowSniffer"};
};

inline auto operator<<(std::ostream& stream, const WinDivertFlowSnifferCallback::FlowKey& key) -> std::ostream& {
  return std::visit(Overload{[&](const auto& key) -> std::ostream& {
                      return stream << WinDivertFlowSnifferCallback::ProtocolToString(key.Proto) << ":"
                                    << key.LocalAddress << ":" << key.LocalPort;
                    }},
                    key);
}

} // namespace gh
