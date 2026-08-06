#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>

namespace gh {

class VpnClientMultiChannelSession;

namespace Interface {

struct Ip4Address {
  static constexpr const size_t kSize = 4;
  std::array<uint8_t, kSize> Bytes;

  auto operator<=>(const Ip4Address&) const = default;
};
struct Ip6Address {
  static constexpr const size_t kSize = 16;
  std::array<uint8_t, kSize> Bytes;

  auto operator<=>(const Ip6Address&) const = default;
};
using IpAddress = std::variant<Ip4Address, Ip6Address>;
template <typename T>
concept AddressTypes = (std::same_as<T, Ip4Address> || std::same_as<T, Ip6Address>);

template <AddressTypes AddressType> struct InterfaceAddress {
  AddressType Address;
  uint8_t PrefixLength;

  auto operator<=>(const InterfaceAddress&) const = default;
};

enum class LogLevel : std::uint8_t { Trace = 0, Debug = 1, Info = 2, Warning = 3, Error = 4, Fatal = 5, Off = 6 };
enum class TunnelState : std::uint8_t { Starting = 0, Running = 1, Stopping = 2, Stopped = 3, Failed = 4 };
enum class PolicyScope : std::uint8_t { SingleProcess, ProcessSubtree };

struct TrafficStats {
  uint64_t ForwardBytes{0};
  uint64_t BackwardBytes{0};
  uint64_t ForwardPackets{0};
  uint64_t BackwardPackets{0};

  void OnForward(uint64_t bytes) {
    ForwardBytes += bytes;
    ForwardPackets++;
  }

  void OnBackword(uint64_t bytes) {
    BackwardBytes += bytes;
    BackwardPackets++;
  }
};

struct VpnTrafficStats : public TrafficStats {
  explicit VpnTrafficStats(const TrafficStats& stats, TunnelState state, int64_t rttMs)
      : TrafficStats(stats), State(state), RttMs(rttMs) {}

  TunnelState State;
  int64_t RttMs;
};

using VpnEndpoint = std::weak_ptr<VpnClientMultiChannelSession>;

class DataPlaneCallbacks {
public:
  explicit DataPlaneCallbacks() = default;
  virtual ~DataPlaneCallbacks() = default;

  DataPlaneCallbacks(const DataPlaneCallbacks&) = delete;
  auto operator=(const DataPlaneCallbacks&) -> DataPlaneCallbacks& = delete;
  DataPlaneCallbacks(DataPlaneCallbacks&&) = delete;
  auto operator=(DataPlaneCallbacks&&) -> DataPlaneCallbacks& = delete;

  virtual void OnVpnStateChanged(TunnelState state, const std::string& message) = 0;
  virtual void OnEndpointStateChanged(VpnEndpoint endpoint, TunnelState state, const std::string& error) = 0;
};

} // namespace Interface
} // namespace gh
