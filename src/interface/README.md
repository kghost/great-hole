# GreatHole Interface Module

This directory defines the public C++23 platform abstraction, shared data types, callback interfaces, and version reporting for GreatHole (`gh::Interface`). It has **zero third-party dependencies** and serves as the stable contract between external hosts (such as UI applications or platform wrappers) and GreatHole's internal engine, VPN data plane, DNS forwarder, and routing policy modules.

---

## 1. CMake Integration

Link against the static library target exposed by `src/interface/CMakeLists.txt`:

- `great-hole-interface`: Standard build.
- `great-hole-interface-asan`: AddressSanitizer build (omitted when `SKIP_ASAN=ON`).

```cmake
target_link_libraries(your_target PRIVATE great-hole-interface)
```

### Windows DLL Symbol Visibility

On Windows (`_WIN32`), `Interface.hpp` controls symbol visibility for `CreatePlatform` via `GREAT_HOLE_INTERFACE_API`:
- Consumers linking against the platform DLL include `Interface.hpp` normally (imports `CreatePlatform` via `__declspec(dllimport)`).
- The platform DLL implementation target (e.g., `great-hole-windows`) defines `GREAT_HOLE_WINDOWS_BUILD_DLL` as a private compile definition to export `CreatePlatform` via `__declspec(dllexport)`.

---

## 2. Public Headers

| Header | Contents |
| :--- | :--- |
| `Interface.hpp` | `PlatformInterface` abstract class, `GREAT_HOLE_INTERFACE_API` macro, and `CreatePlatform()` factory declaration. Automatically includes `InterfaceWin32.hpp` on `_WIN32`. |
| `InterfaceCommonTypes.hpp` | Cross-platform primitives: IP address types, enums (`LogLevel`, `TunnelState`, `PolicyScope`), traffic stats, `VpnEndpoint`, `PolicyRule`, `DataPlaneCallbacks`, DNS forwarder configuration, and `DnsForwarderCallbacks`. |
| `InterfaceWin32.hpp` | Windows-only `PlatformInterfaceWin32` extension interface and Windows network/process/flow introspection structs. |

---

## 3. Core Platform API (`PlatformInterface`)

### Factory & Version

```cpp
namespace gh::Interface {
  auto CreatePlatform(DataPlaneCallbacks& callbacks) -> std::shared_ptr<PlatformInterface>;
}
```

- **`CreatePlatform(callbacks)`**: Instantiates the platform-specific implementation. The caller must ensure `callbacks` outlives the returned `PlatformInterface` (or until `StopEngine()` completes).
- **`GetVersion() -> std::string`**: Returns the build-time version string captured from `git describe --tags` (or `"unknown"` if Git metadata was unavailable at configure time).

### Engine & VPN Lifecycle

- **`StartEngine() -> std::error_code`**: Initializes core backend services (event loop, policy engine, interface monitor, and data plane context).
- **`StopEngine() -> std::error_code`**: Tears down all backend services and releases engine-lifetime resources.
- **`StartVpn(std::span<IpAddress> addresses, int32_t mtu, std::span<uint8_t> encryption_key) -> std::error_code`**: Starts the VPN tunnel data plane with the assigned local IP addresses, MTU, and symmetric encryption key.
- **`StopVpn() -> std::error_code`**: Stops the active VPN data plane while preserving registered endpoints for subsequent `StartVpn()` calls.

### VPN Endpoint Management

- **`static constexpr size_t kPskSize = 16`** and **`using PskType = std::array<uint8_t, kPskSize>`**.
- **`AddEndpoint(const PskType& psk, const std::string& address) -> VpnEndpoint`**: Registers a remote VPN server endpoint with its 16-byte pre-shared key and returns an opaque handle (`VpnEndpoint`).
- **`RemoveEndpoint(VpnEndpoint endpoint)`**: Unregisters and destroys a previously added endpoint.
- **`StartEndpoint(VpnEndpoint endpoint)`** / **`StopEndpoint(VpnEndpoint endpoint)`**: Starts or stops traffic on a registered endpoint.
- **`GetTrafficStats(VpnEndpoint endpoint) -> std::optional<VpnTrafficStats>`**: Returns current byte/packet counters, tunnel state, and RTT (in ms) for the endpoint, or `std::nullopt` if the handle is expired or inactive.

### DNS Forwarder

- **`StartDnsForwarder(const DnsForwarderConfiguration& config, DnsForwarderCallbacks& callbacks) -> std::error_code`**: Starts the local DNS forwarder with the given listeners, upstream pools, and domain routes. `callbacks` must remain valid until `StopDnsForwarder()` returns.
- **`StopDnsForwarder() -> std::error_code`**: Stops the DNS forwarder.
- **`GetDnsForwarderConfiguration() -> DnsForwarderConfiguration`**: Retrieves the currently active DNS forwarder configuration.

### Logging

- **`SetLogLevel(LogLevel level)`**: Sets the global logging verbosity (`Trace`, `Debug`, `Info`, `Warning`, `Error`, `Fatal`, `Off`).

---

## 4. Windows Platform Extension (`PlatformInterfaceWin32`)

On `_WIN32`, `PlatformInterface` inherits from `PlatformInterfaceWin32`, adding process-aware routing policy control, runtime introspection, and per-adapter DNS override APIs.

### Routing Policy Control

- **`ClearPathRegistry()`**: Removes all executable path-based routing policies.
- **`AddPathPolicy(const std::string& path, const PolicyRule& policy)`**: Associates a routing policy with an executable path.
- **`RemovePathPolicy(const std::string& path)`**: Removes the policy for a specific executable path.
- **`GetAllPolicies() -> std::unordered_map<std::string, PolicyRule>`**: Returns a snapshot of all registered path policies.
- **`AddProcessPolicy(ProcessSequence process, const PolicyRule& policy) -> std::expected<void, std::string>`**: Assigns a routing policy to a running process identified by its `ProcessSequence`.
- **`SetDefaultAction(const PolicyRule::RoutingAction& action)`** / **`GetDefaultAction() -> PolicyRule::RoutingAction`**: Sets or queries the fallback routing action for unmatched traffic.
- **`LaunchWithPolicy(const std::string& imagePath, const std::optional<std::string>& commandLine, const PolicyRule& policy) -> std::expected<ProcessSequence, std::string>`**: Launches a new process with a routing policy attached from startup, returning its `ProcessSequence`.

### Runtime Introspection

- **`GetFlows() -> std::vector<FlowInfo>`**: Returns currently tracked socket-to-PID flow mappings.
- **`GetConnections() -> std::vector<TrackedConnectionInfo>`**: Returns active tracked connections and their policy marks.
- **`GetProcessTree() -> std::vector<ProcessInfo>`**: Returns the tracked process tree with parent relationships and attached policies.
- **`GetInterfaces(bool refresh) -> std::vector<InterfaceInfo>`**: Returns Windows network adapter information (LUID, index, name, IPv4/IPv6 addresses, MTU, metric, and current/original DNS servers). Pass `refresh = true` to force a live OS query.

### Adapter DNS Overrides & Component Logging

- **`OverrideDnsServers(InterfaceLuid interfaceLuid, const std::vector<IpAddress>& dnsServers) -> std::expected<void, std::string>`**: Overrides DNS servers on the network adapter identified by its 64-bit `InterfaceLuid`, preserving original settings for restoration.
- **`RestoreDnsServers(InterfaceLuid interfaceLuid) -> std::expected<void, std::string>`**: Restores previously overridden DNS servers on the specified adapter.
- **`SetProcessTreeTrackerLogLevel(LogLevel level)`** / **`SetFlowTrackerLogLevel(LogLevel level)`** / **`SetPolicySelectorLogLevel(LogLevel level)`**: Sets fine-grained log levels for individual Windows policy engine components.

---

## 5. Key Shared Types & Callbacks

### Network Addresses
- `Ip4Address` (`std::array<uint8_t, 4> Bytes`), `Ip6Address` (`std::array<uint8_t, 16> Bytes`), and `IpAddress = std::variant<Ip4Address, Ip6Address>`.
- `AddressTypes` concept constrains templates to `Ip4Address` or `Ip6Address`.
- `InterfaceAddress<AddressType>` pairs an `Address` with a CIDR `PrefixLength`.

### Routing Policies (`PolicyRule`)
A `PolicyRule` combines a `PolicyScope` (`SingleProcess` or `ProcessSubtree`) with a `RoutingAction` variant:
- `PolicyRule::ByPassRoute`: Send traffic directly, bypassing the VPN.
- `PolicyRule::DiscardRoute`: Drop matching traffic.
- `PolicyRule::EndpointRoute`: Route traffic through a specific `VpnEndpoint`.
- `PolicyRule::VpnSplitRoute`: Route using internal per-port split rules (`std::weak_ptr<gh::VpnSplitRule>`).

### Traffic Statistics
- `TrafficStats`: Tracks `ForwardBytes`, `BackwardBytes`, `ForwardPackets`, `BackwardPackets`, with helper mutators `OnForward(uint64_t bytes)` and `OnBackword(uint64_t bytes)`.
- `VpnTrafficStats`: Extends `TrafficStats` with `TunnelState State` and `int64_t RttMs`.

### Callback Interfaces
Both callback interfaces are non-copyable and non-movable abstract classes:
- **`DataPlaneCallbacks`**:
  - `OnVpnStateChanged(TunnelState state, const std::string& message)`
  - `OnEndpointStateChanged(VpnEndpoint endpoint, TunnelState state, const std::string& error)`
- **`DnsForwarderCallbacks`**:
  - `OnDnsQueryResult(const std::string& upstream, const std::string& domain, DnsQueryResult results)` where `DnsQueryResult` is `std::variant<std::span<const Ip4Address>, std::span<const Ip6Address>>`.

---

## 6. Minimal Usage Example

```cpp
#include "Interface.hpp"

class MyCallbacks : public gh::Interface::DataPlaneCallbacks {
public:
  void OnVpnStateChanged(gh::Interface::TunnelState state, const std::string& message) override {
    // Handle VPN state transitions
  }
  void OnEndpointStateChanged(gh::Interface::VpnEndpoint endpoint, gh::Interface::TunnelState state,
                              const std::string& error) override {
    // Handle per-endpoint state transitions
  }
};

void RunExample() {
  MyCallbacks callbacks;
  auto platform = gh::Interface::CreatePlatform(callbacks);

  platform->SetLogLevel(gh::Interface::LogLevel::Info);
  if (auto ec = platform->StartEngine(); ec) {
    return;
  }

  gh::Interface::PlatformInterface::PskType psk{ /* 16-byte key */ };
  auto endpoint = platform->AddEndpoint(psk, "203.0.113.10:51820");

  std::array<gh::Interface::IpAddress, 1> addrs = {
      gh::Interface::Ip4Address{{10, 0, 0, 2}}
  };
  std::array<uint8_t, 32> key{ /* encryption key */ };

  if (auto ec = platform->StartVpn(addrs, 1400, key); !ec) {
    platform->StartEndpoint(endpoint);
    // ... active session ...
    platform->StopEndpoint(endpoint);
    platform->StopVpn();
  }

  platform->RemoveEndpoint(endpoint);
  platform->StopEngine();
}
```
