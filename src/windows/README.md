# GreatHole Windows Platform Interface

This directory houses the C++ implementation of the GreatHole Platform Interface (`gh::Interface::PlatformInterface`) for Windows. 

The implementation manages the lifecycle of the packet-redirection backend, the multi-channel VPN client, and the Windows Routing Policy Engine using a thread-safe asynchronous architecture.

---

## 1. Module Overview

The platform interface exposes a synchronous interface to consumers while running the core network components asynchronously inside a Boost.Asio event loop (integrated with Omni fibers).

On Windows, the platform module enables:
1. **Lifecycle Control**: Starting and stopping the VPN data plane (`TunnelDataPlane`) using WinDivert packet capture.
2. **Channel Management**: Registering and managing VPN session endpoints.
3. **Application Routing Policies**: Dynamically applying path-based and process-based redirection/bypass rules to the Windows Policy Engine.

---

## 2. Platform Interface Specification

The interface is defined in `src/interface/Interface.hpp`:

```cpp
class PlatformInterface {
public:
  virtual ~PlatformInterface() = default;

  virtual void Start(int32_t mtu, std::span<uint8_t> encryption_key) = 0;
  virtual void Stop() = 0;

  virtual auto AddEndpoint(const std::string& psk, const std::string& address) -> VpnEndpoint = 0;
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
};

// Factory function to create the platform-specific implementation
auto CreatePlatform(DataPlaneCallbacks& callbacks) -> std::shared_ptr<PlatformInterface>;
```

---

## 3. Implementation Details (`PlatformImpl`)

The Windows-specific implementation (`PlatformImpl` in `Interface.cpp`):
- Inherits from `gh::Interface::PlatformInterface` and `gh::DeferredPacketInjector`.
- Spawns a dedicated worker thread (`_AsioThread`) running a `boost::asio::io_context` and `Omni::Fiber::Manager` to execute async operations.
- Uses `Omni::Fiber::EventQueue` to safely marshal calls from the caller's thread onto the internal Boost.Asio thread and block appropriately using `std::promise` and `std::future`.
