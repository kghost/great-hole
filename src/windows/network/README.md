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
- **`InterfaceAddress<AddressType>`**:
  - `AddressType Address`: `boost::asio::ip::address_v4` or `address_v6`.
  - `uint8_t PrefixLength`: Network prefix length (subnet mask bits).
  - `operator<=>`: Defaulted three-way comparison operator.
- **`DnsInfo<AddressType>`**:
  - `std::vector<AddressType> Servers`: Configured DNS server IP addresses.
  - `bool IsDhcp`: True if DNS servers are automatically assigned via DHCP / RDNSS.
- **`IpInterfaceCommon`**:
  - `uint32_t NlMtu`: Network layer MTU.
  - `uint32_t Metric`: Interface route metric.
  - `bool Connected`: Operational link status.
  - `bool ForwardingEnabled`: IPv4 or IPv6 packet forwarding enabled status.
- **`IpInterfaceInfo<AddressType>`**:
  - `IpInterfaceCommon Common`: Interface properties common to IP address family.
  - `std::set<InterfaceAddress<AddressType>> Addresses`: Assigned IPv4 or IPv6 unicast addresses and prefix lengths.
  - `DnsInfo<AddressType> DnsServers`: Configured DNS server IP addresses and DHCP state.
  - `std::optional<DnsInfo<AddressType>> OriginalDnsServers`: Original DNS servers and DHCP state captured prior to any override.
- **`InterfaceInfo`**:
  - `InterfaceKey InterfaceIndex`: Windows interface index (`NET_IFINDEX`).
  - `NET_LUID InterfaceLuid`: Interface LUID value (`NET_LUID`).
  - `std::optional<std::string> InterfaceName`: Interface alias name.
  - `std::optional<IpInterfaceInfo<boost::asio::ip::address_v4>> V4Info`: IPv4 configuration and assigned addresses.
  - `std::optional<IpInterfaceInfo<boost::asio::ip::address_v6>> V6Info`: IPv6 configuration and assigned addresses.

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

// Re-query interfaces and addresses while preserving OriginalDnsServers
monitor->Refresh();

// Override interface DNS servers (returns std::expected<void, std::string>)
auto overrideRes = monitor->OverrideDnsServers(ifIndex, newDnsServers);

// Restore interface DNS servers (returns std::expected<void, std::string>)
auto restoreRes = monitor->RestoreDnsServers(ifIndex);

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
