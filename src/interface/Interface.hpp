#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#if defined(GREAT_HOLE_WINDOWS_BUILD_DLL)
#define GREAT_HOLE_INTERFACE_API __declspec(dllexport)
#else
#define GREAT_HOLE_INTERFACE_API __declspec(dllimport)
#endif
#else
#define GREAT_HOLE_INTERFACE_API
#endif

// Forward declaration of generated Rust functions
namespace gh {

class VpnClientMultiChannelSession;

namespace Interface {

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
  VpnTrafficStats() = default;
  explicit VpnTrafficStats(const TrafficStats& stats, int64_t rttMs = -1) : TrafficStats(stats), RttMs(rttMs) {}

  int64_t RttMs{-1};
};

struct FlowInfo {
  std::string Protocol;
  std::string LocalAddress;
  std::string RemoteAddress;
  uint16_t LocalPort{0};
  uint16_t RemotePort{0};
  uint32_t ProcessId{0};
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
  virtual void OnTunnelStateChanged(VpnEndpoint endpoint, TunnelState state, const std::string& error) = 0;
};

// Interface for the Tunnel Data Plane and Policy Engine
class PlatformInterface {
public:
  explicit PlatformInterface() = default;
  virtual ~PlatformInterface() = default;

  PlatformInterface(const PlatformInterface&) = delete;
  auto operator=(const PlatformInterface&) -> PlatformInterface& = delete;
  PlatformInterface(PlatformInterface&&) = delete;
  auto operator=(PlatformInterface&&) -> PlatformInterface& = delete;

  virtual auto StartEngine() -> std::error_code = 0;
  virtual auto StartVpn(int32_t mtu, std::span<uint8_t> encryption_key) -> std::error_code = 0;
  virtual auto StopEngine() -> std::error_code = 0;
  virtual auto StopVpn() -> std::error_code = 0;

  virtual auto AddEndpoint(const std::array<uint8_t, 16>& psk, const std::string& address) -> VpnEndpoint = 0;
  virtual void RemoveEndpoint(VpnEndpoint endpoint) = 0;

  // Policy Interface
  virtual void ClearRegistry() = 0;
  virtual void AddPathBypassRule(const std::string& path, PolicyScope scope) = 0;
  virtual void AddPathEndpointRule(const std::string& path, VpnEndpoint endpoint, PolicyScope scope) = 0;
  virtual void RemovePathRule(const std::string& path) = 0;
  virtual void AddPidEndpointRule(uint32_t pid, VpnEndpoint endpoint, PolicyScope scope) = 0;
  virtual void SetDefaultEndpoint(VpnEndpoint endpoint) = 0;
  virtual void SetDefaultBypass() = 0;
  virtual void LaunchWithPolicy(const std::string& command_line, VpnEndpoint endpoint, PolicyScope scope) = 0;
  virtual auto GetFlows() -> std::vector<FlowInfo> = 0;
};

GREAT_HOLE_INTERFACE_API auto CreatePlatform(DataPlaneCallbacks& callbacks) -> std::shared_ptr<PlatformInterface>;

} // namespace Interface
} // namespace gh
