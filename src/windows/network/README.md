# Windows Network Interface & IP Address Monitor (`src/windows/network`)

The `src/windows/network` module provides an event-driven service (`InterfaceMonitor`) to query and continuously monitor Windows network interfaces and their assigned IPv4/IPv6 addresses.

---

## 1. Overview & Public API

The module is housed in namespace `gh::windows::network`.

### Data Structures

- **`IpAddressInfo`**:
  - `boost::asio::ip::address Address`: IPv4 or IPv6 address.
  - `uint8_t PrefixLength`: Network prefix length (subnet mask bits).
  - `bool IsUnicast`: True for unicast addresses.
- **`InterfaceInfo`**:
  - `uint32_t Index`: Windows interface index (`NET_IFINDEX`).
  - `uint64_t Luid`: Interface LUID value (`NET_LUID`).
  - `std::string Name`: Adapter GUID or device identifier (e.g. `{GUID}`).
  - `std::string FriendlyName`: Human-readable adapter name (e.g. `Ethernet`, `Wi-Fi`).
  - `std::string Description`: Device hardware description.
  - `std::string MacAddress`: Formatted MAC address string (e.g. `00-11-22-33-44-55`).
  - `uint32_t Type`: Interface type (`IFTYPE`, e.g. `IF_TYPE_ETHERNET_CSMACD`, `IF_TYPE_SOFTWARE_LOOPBACK`).
  - `uint32_t OperStatus`: Operational status (`IF_OPER_STATUS`).
  - `bool IsUp`: Convenience boolean indicating if `OperStatus == IfOperStatusUp`.
  - `uint32_t Mtu`: Maximum transmission unit in bytes.
  - `std::vector<IpAddressInfo> IpAddresses`: List of assigned unicast IP addresses.

### Observer Callback Interface

Components implement `InterfaceMonitorCallback` to receive asynchronous coroutine notifications whenever interface or address changes occur:

```cpp
class InterfaceMonitorCallback {
public:
  virtual auto OnInterfacesChanged(const std::vector<InterfaceInfo>& interfaces) -> Omni::Fiber::Coroutine<void> = 0;
};
```

### `InterfaceMonitor` Service

`InterfaceMonitor` inherits from `gh::ServiceBase` and manages background Windows IP Helper notifications.

```cpp
#include "InterfaceMonitor.hpp"

// Instantiation
auto monitor = std::make_shared<gh::windows::network::InterfaceMonitor>(ioContext.get_executor(), callback);

// Lifecycle management
co_await monitor->Start();

// Query active cached snapshot
std::vector<gh::windows::network::InterfaceInfo> interfaces = monitor->GetInterfaces();

// Manually trigger a refresh update
co_await monitor->Refresh();

// Stop service
co_await monitor->Stop();
```

---

## 2. Command Line Tool (`net-interface-dump`)

A built-in command line utility is provided under `tools/`:

```powershell
# Display current network interfaces snapshot
./net-interface-dump.exe

# Continuously watch and report real-time interface and IP address changes
./net-interface-dump.exe --watch
```
