#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "InterfaceCommonTypes.hpp"

namespace gh::Interface {

struct IpInterfaceCommon {
  uint32_t NlMtu{0};
  uint32_t Metric{0};
  bool Connected{false};
  bool ForwardingEnabled{false};
};

template <AddressTypes AddressType> struct DnsInfo {
  std::vector<AddressType> Servers;
  bool IsDhcp{false};
  auto operator<=>(const DnsInfo&) const = default;
};

template <AddressTypes AddressType> struct IpInterfaceInfo {
  IpInterfaceCommon Common;
  std::set<InterfaceAddress<AddressType>> Addresses;
  DnsInfo<AddressType> DnsServers;
  std::optional<DnsInfo<AddressType>> OriginalDnsServers;
};

struct InterfaceInfo {
  uint32_t InterfaceIndex{0};
  uint64_t InterfaceLuid{0};
  std::optional<std::string> InterfaceName;
  std::optional<IpInterfaceInfo<Ip4Address>> V4Info;
  std::optional<IpInterfaceInfo<Ip6Address>> V6Info;
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

// Interface for the Tunnel Data Plane and Policy Engine
class PlatformInterfaceWin32 {
public:
  explicit PlatformInterfaceWin32() = default;
  virtual ~PlatformInterfaceWin32() = default;

  PlatformInterfaceWin32(const PlatformInterfaceWin32&) = delete;
  auto operator=(const PlatformInterfaceWin32&) -> PlatformInterfaceWin32& = delete;
  PlatformInterfaceWin32(PlatformInterfaceWin32&&) = delete;
  auto operator=(PlatformInterfaceWin32&&) -> PlatformInterfaceWin32& = delete;

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
  virtual auto GetInterfaces(bool refresh) -> std::vector<InterfaceInfo> = 0;

  // Logging Interface
  virtual void SetProcessTreeTrackerLogLevel(LogLevel level) = 0;
  virtual void SetFlowTrackerLogLevel(LogLevel level) = 0;
  virtual void SetPolicySelectorLogLevel(LogLevel level) = 0;
};

} // namespace gh::Interface
