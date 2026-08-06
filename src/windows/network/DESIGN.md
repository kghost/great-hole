# Windows Network Interface & IP Address Monitor Design Specification (`src/windows/network`)

## 1. Architecture Overview

The `InterfaceMonitor` module provides non-blocking, event-driven network interface and IP address monitoring on Windows hosts.

```
+-------------------------------------------------------------------+
|                        Windows OS Kernel                          |
|   (IP Helper / netioapi: NotifyIpInterfaceChange / AddressChange)  |
+-------------------------------------------------------------------+
                                  |
                                  v  (OS ThreadPool Callback)
+-------------------------------------------------------------------+
|              OnInterfaceOrAddressChangedCallback                  |
+-------------------------------------------------------------------+
                                  |
                   Pushes task into ExternalQueue
                                  |
                                  v
+-------------------------------------------------------------------+
|                     InterfaceMonitor (ServiceBase)                |
|  - Fiber event loop in DoWork()                                   |
|  - Queries GetAdaptersAddresses                                   |
|  - Compares interface state diffs                                 |
|  - Invokes InterfaceMonitorCallback::OnInterfacesChanged          |
+-------------------------------------------------------------------+
```

---

## 2. Windows IP Helper Integration

### 2.1 Interface & Address Discovery (`GetAdaptersAddresses`)

- Uses `GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_ALL_INTERFACES, ...)` to retrieve IPv4 and IPv6 adapters.
- Friendly names and descriptions are provided in wide strings (`PWCHAR`) and safely converted to UTF-8 using `gh::ToString`.
- Unicast IP addresses are enumerated via `FirstUnicastAddress`. Sockaddr pointers (`sockaddr_in` / `sockaddr_in6`) are converted directly to `boost::asio::ip::address_v4` and `boost::asio::ip::address_v6`. `OnLinkPrefixLength` provides the subnet prefix length.

### 2.2 Event Notifications (`netioapi.h`)

- `NotifyIpInterfaceChange` registers interest in interface state changes (e.g. adapter connect/disconnect, link up/down, MTU or metric updates). `OnIpInterfaceChangedCallback` receives `PMIB_IPINTERFACE_ROW` and pushes an `InterfaceChangeEvent` into `_TaskQueue`.
- `NotifyUnicastIpAddressChange` registers interest in IPv4/IPv6 address assignments, dynamic DHCP changes, or IPv6 SLAAC address updates. `OnUnicastIpAddressChangedCallback` receives `PMIB_UNICASTIPADDRESS_ROW` and pushes an `AddressChangeEvent` into `_TaskQueue`.
- Notifications arrive on Windows ThreadPool threads. The callbacks push notification tasks into `Omni::Fiber::ExternalQueue<Task>`, avoiding raw thread synchronization issues and safely waking up `InterfaceMonitor::DoWork()` inside the fiber context.

### 2.3 IP Address Synchronization in `IpInterfaceInfo`

- **Initial Query (`QueryAddresses`)**: Iterates all unicast IP addresses returned by `GetAdaptersAddresses`. Beyond populating `_Addresses`, `QueryAddresses` populates the `Addresses` set (`std::set<InterfaceAddress<AddressType>>`) within `_Interfaces[ifIndex].V4Info` (IPv4) and `_Interfaces[ifIndex].V6Info` (IPv6). `InterfaceAddress` provides a defaulted `operator<=>` for ordered set storage.
- **Event-Driven Synchronization (`HandleAddressChangeEvent`)**: On `MibAddInstance`, `MibDeleteInstance`, or `MibParameterNotification` events, `HandleAddressChangeEvent` dynamically updates both `_Addresses` and the corresponding `Addresses` set in `_Interfaces[ifIndex].V4Info` or `_Interfaces[ifIndex].V6Info`. Address movement between interfaces or prefix length updates are handled by removing stale entries before inserting new ones.

### 2.4 DNS Server Overriding (`OverrideDnsServers`)

- **Original DNS Preservation**: Before modifying DNS configuration, `OverrideDnsServers` checks if `OriginalDnsServers` (`std::optional<std::vector<AddressType>>`) has a value. If uninitialized (`!OriginalDnsServers.has_value()`), current `DnsServers` are saved into `OriginalDnsServers`. If `OriginalDnsServers` already has a value, this step is skipped to preserve initial adapter state across multiple override calls.
- **OS Configuration**: Uses `SetInterfaceDnsSettings` from `netioapi.h` with `DNS_INTERFACE_SETTINGS_VERSION1` and `DNS_SETTING_NAMESERVER` (plus `DNS_SETTING_IPV6` for IPv6) to configure system DNS servers for the target interface.
- **Cache Update**: Updates `DnsServers` in `IpInterfaceInfo` with the new target DNS servers.

---

## 3. Concurrency & Fiber Synchronization

1. **State Protection**: Interface entries (`_Interfaces`) and IP address mappings (`_Addresses`) are managed within the service. `GetInterfaces()` returns `_Interfaces` and `GetAddressInfo()` returns `_Addresses`.
2. **Fiber Integration**: The worker fiber in `DoWork()` awaits on both service cancellation (`_Stop.GetFiberCancelEvent()`) and `_TaskQueue` using `Omni::Fiber::Select`.
3. **Event Processing**: Queue events (`AddressChangeEvent` / `InterfaceChangeEvent`) are processed inside `DoWork()` using `std::visit`, extracting updated row attributes (`GetIpInterfaceEntry` / `GetUnicastIpAddressEntry`) and maintaining cached snapshots while logging changes.
