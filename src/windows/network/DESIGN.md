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

- **Interface Keying by LUID**: `InterfaceMonitor` indexes `_Interfaces` by persistent 64-bit `InterfaceLuid` (`NET_LUID::Value`). Because both IPv4 and IPv6 network layers on Windows share the exact same adapter LUID, this keying reliably merges dual-stack configurations into a single unified `InterfaceInfo` record containing both `V4Info` and `V6Info`.
- **Initial Query (`QueryAddresses`)**: Iterates all unicast IP addresses returned by `GetAdaptersAddresses`. Beyond populating `_Addresses`, `QueryAddresses` populates the `Addresses` set (`std::set<InterfaceAddress<AddressType>>`) within `_Interfaces[luid].V4Info` (IPv4) and `_Interfaces[luid].V6Info` (IPv6). `InterfaceAddress` provides a defaulted `operator<=>` for ordered set storage.
- **Event-Driven Synchronization (`HandleAddressChangeEvent`)**: On `MibAddInstance`, `MibDeleteInstance`, or `MibParameterNotification` events, `HandleAddressChangeEvent` dynamically updates both `_Addresses` and the corresponding `Addresses` set in `_Interfaces[luid].V4Info` or `_Interfaces[luid].V6Info`. Address movement between interfaces or prefix length updates are handled by converting the event or row interface information to LUID and removing stale entries before inserting new ones.

### 2.4 DNS Server Overriding & Restoration (`OverrideDnsServers` / `RestoreDnsServers`)

- **Interface Identification by LUID**: DNS overriding and restoring operations take `InterfaceLuid` (`uint64_t`) as their primary interface identifier. The 64-bit LUID is converted to `GUID` via `ConvertInterfaceLuidToGuid` before interacting with the system DNS settings.
- **Unified Single API for IPv4 & IPv6**: `OverrideDnsServers` accepts a single list `std::vector<Interface::IpAddress>` which may contain IPv4 addresses (`Ip4Address`), IPv6 addresses (`Ip6Address`), or both. The method partitions addresses by family and overrides IPv4 and/or IPv6 DNS servers as appropriate on the target adapter in a single call.
- **Original DNS Preservation**: Before modifying DNS configuration, `OverrideDnsServers` checks if `OriginalDnsServers` (`std::optional<DnsInfo<AddressType>>`) has a value. If uninitialized (`!OriginalDnsServers.has_value()`), current `DnsServers` are saved into `OriginalDnsServers`. If `OriginalDnsServers` already has a value, this step is skipped to preserve initial adapter state across multiple override calls.
- **OS Configuration**: Uses `SetInterfaceDnsSettings` from `netioapi.h` with `DNS_INTERFACE_SETTINGS_VERSION1` and `DNS_SETTING_NAMESERVER` (plus `DNS_SETTING_IPV6` for IPv6) to configure system DNS servers for the target interface.
- **Cache Update**: Updates `DnsServers` in `IpInterfaceInfo` with the new target DNS servers upon successful configuration.
- **Restoration**: `RestoreDnsServers` retrieves the captured `OriginalDnsServers`, restores them using `SetInterfaceDnsSettings`, resets `OriginalDnsServers` to `std::nullopt`, and restores DHCP or static settings accordingly.
- **Automatic RAII Cleanup in Destructor**: In `InterfaceMonitor::~InterfaceMonitor()`, the monitor iterates over all tracked interfaces and automatically calls `RestoreDnsServers(luid)` for any interface that has active `OriginalDnsServers` (IPv4 or IPv6), ensuring no DNS overrides leak if the service or application terminates without explicit restoration.

---

## 3. Concurrency & Fiber Synchronization

1. **State Protection**: Interface entries (`_Interfaces`) and IP address mappings (`_Addresses`) are managed within the service. `GetInterfaces()` returns `_Interfaces` and `GetAddressInfo()` returns `_Addresses`.
2. **Fiber Integration**: The worker fiber in `DoWork()` awaits on both service cancellation (`_Stop.GetFiberCancelEvent()`) and `_TaskQueue` using `Omni::Fiber::Select`.
3. **Event Processing**: Queue events (`AddressChangeEvent` / `InterfaceChangeEvent`) are processed inside `DoWork()` using `std::visit`, extracting updated row attributes (`GetIpInterfaceEntry` / `GetUnicastIpAddressEntry`) and maintaining cached snapshots while logging changes.
