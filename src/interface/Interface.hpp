#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <unordered_map>
#include <variant>
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

struct Ip4Address {
  static constexpr const size_t kSize = 4;
  std::array<uint8_t, kSize> Bytes;
};
struct Ip6Address {
  static constexpr const size_t kSize = 16;
  std::array<uint8_t, kSize> Bytes;
};
using IpAddress = std::variant<Ip4Address, Ip6Address>;

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

struct FlowConnection {
  std::string Protocol;
  std::string LocalAddress;
  std::string RemoteAddress;
  uint16_t LocalPort{0};
  uint16_t RemotePort{0};
};

using ProcessId = uint32_t;
using ProcessSequence = uint64_t;

struct FlowInfo {
  std::string Protocol;
  std::string LocalAddress;
  uint16_t LocalPort{0};
  ProcessId Process{0};
};

using VpnEndpoint = std::weak_ptr<VpnClientMultiChannelSession>;

struct PolicyRule {
  struct ByPassRoute {};
  struct DiscardRoute {};
  struct EndpointRoute {
    VpnEndpoint Endpoint;
  };

  using RoutingAction = std::variant<ByPassRoute, DiscardRoute, EndpointRoute>;

  RoutingAction Action;
  PolicyScope Scope = PolicyScope::SingleProcess;
};

struct ProcessInfo {
  ProcessSequence Process{0};
  std::optional<ProcessSequence> ParentProcess{0};
  ProcessId InfoProcessId{0};
  std::optional<PolicyRule> Policy;
};

struct TrackedConnectionInfo {
  FlowConnection Connection;
  std::string Mark;
};

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

// Interface for the Tunnel Data Plane and Policy Engine
class PlatformInterface {
public:
  explicit PlatformInterface() = default;
  virtual ~PlatformInterface() = default;

  PlatformInterface(const PlatformInterface&) = delete;
  auto operator=(const PlatformInterface&) -> PlatformInterface& = delete;
  PlatformInterface(PlatformInterface&&) = delete;
  auto operator=(PlatformInterface&&) -> PlatformInterface& = delete;

  virtual auto GetVersion() -> std::string;

  virtual auto StartEngine() -> std::error_code = 0;
  virtual auto StopEngine() -> std::error_code = 0;

  virtual auto StartVpn(std::span<IpAddress> addresses, int32_t mtu, std::span<uint8_t> encryption_key)
      -> std::error_code = 0;
  virtual auto StopVpn() -> std::error_code = 0;

  static constexpr size_t kPskSize = 16;
  using PskType = std::array<uint8_t, kPskSize>;
  virtual auto AddEndpoint(const PskType& psk, const std::string& address) -> VpnEndpoint = 0;
  virtual void RemoveEndpoint(VpnEndpoint endpoint) = 0;

  virtual void StartEndpoint(VpnEndpoint endpoint) = 0;
  virtual void StopEndpoint(VpnEndpoint endpoint) = 0;

  virtual auto GetTrafficStats(VpnEndpoint endpoint) -> std::optional<VpnTrafficStats> = 0;

  // Policy Interface
  virtual void ClearPathRegistry() = 0;
  virtual void AddPathPolicy(const std::string& path, const PolicyRule& policy) = 0;
  virtual void RemovePathPolicy(const std::string& path) = 0;
  virtual auto GetAllPolicies() -> std::unordered_map<std::string, PolicyRule> = 0;
  virtual auto AddProcessPolicy(ProcessSequence process, const PolicyRule& policy)
      -> std::expected<void, std::string> = 0;
  virtual void SetDefaultAction(const PolicyRule::RoutingAction& action) = 0;
  virtual auto GetDefaultAction() -> PolicyRule::RoutingAction = 0;
  virtual auto LaunchWithPolicy(const std::string& imagePath, const std::optional<std::string>& commandLine,
                                const PolicyRule& policy) -> std::expected<ProcessSequence, std::string> = 0;
  virtual auto GetFlows() -> std::vector<FlowInfo> = 0;
  virtual auto GetConnections() -> std::vector<TrackedConnectionInfo> = 0;
  virtual auto GetProcessTree() -> std::vector<ProcessInfo> = 0;

  // Logging Interface
  virtual void SetLogLevel(LogLevel level) = 0;
  virtual void SetProcessTreeTrackerLogLevel(LogLevel level) = 0;
  virtual void SetFlowTrackerLogLevel(LogLevel level) = 0;
  virtual void SetPolicySelectorLogLevel(LogLevel level) = 0;
};

GREAT_HOLE_INTERFACE_API auto CreatePlatform(DataPlaneCallbacks& callbacks) -> std::shared_ptr<PlatformInterface>;

} // namespace Interface
} // namespace gh
