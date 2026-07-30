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

- `NotifyIpInterfaceChange` registers interest in interface state changes (e.g. adapter connect/disconnect, link up/down).
- `NotifyUnicastIpAddressChange` registers interest in IPv4/IPv6 address assignments, dynamic DHCP changes, or IPv6 SLAAC address updates.
- Notifications arrive on Windows ThreadPool threads. The callbacks push notification tasks into `Omni::Fiber::ExternalQueue<Task>`, avoiding raw thread synchronization issues and safely waking up `InterfaceMonitor::DoWork()` inside the fiber context.

---

## 3. Concurrency & Fiber Synchronization

1. **State Protection**: `_Interfaces` snapshot and `_Callback` pointer are protected by `std::mutex _Mutex`. `GetInterfaces()` acquires `_Mutex` and returns a copy of `_Interfaces`.
2. **Fiber Integration**: The worker fiber in `DoWork()` awaits on both service cancellation (`_Stop.GetFiberCancelEvent()`) and `_TaskQueue` using `Omni::Fiber::Select`.
3. **Deduplication**: Rapid back-to-back OS callback triggers drain all queued task items before performing a single `QueryInterfaces()` scan and state equality comparison.
