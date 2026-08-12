# DNS Forwarder Module (`src/dns`)

The `dns` module provides a multi-upstream DNS Forwarder service (`DnsForwarder`) for GreatHole. It acts as a standard DNS server accepting inbound client queries (over UDP/TCP) and routes requests to different upstream DNS instances (`DnsUpstream`) based on domain and subdomain matching rules.

---

## Key Features

1. **Standard DNS Server**: Accepts standard DNS queries on configurable listening endpoints (e.g., `127.0.0.1:53`).
2. **Domain-Based Upstream Routing**: Flexible `DnsRouter` that maps query domains (domain suffix matching, exact domain match, default fallback) to distinct `DnsUpstream` instances.
3. **Fixed Ephemeral Source Ports**: Each `DnsUpstream` binds its UDP socket to local port `0` at construction time. The OS assigns an ephemeral port that remains **fixed for the lifetime of the upstream**, allowing network-layer filters (e.g., WinDivert, TUN, iptables) to identify traffic originating from that upstream.
4. **OmniFiber Integration**: Fully async execution integrated with GreatHole's `ServiceBase` and `omni-fiber` concurrency framework.

---

## Architecture Overview

```
[ DNS Client / OS ]
        │ (UDP:53)
        ▼
┌─────────────────────────────────────────────────────────┐
│                      DnsForwarder                       │
│  ┌───────────────────────────────────────────────────┐  │
│  │                    DnsRouter                      │  │
│  └───────┬───────────────────┬───────────────────┬───┘  │
│          │ google.com        │ internal          │ default
│          ▼                   ▼                   ▼      │
│  ┌───────────────┐   ┌───────────────┐   ┌───────────┐  │
│  │ DnsUpstream 1 │   │ DnsUpstream 2 │   │DnsUpstream3│ │
│  │  (Ephemeral)  │   │  (Ephemeral)  │   │(...)      │  │
│  └───────┬───────┘   └───────┬───────┘   └─────┬─────┘  │
└──────────┼───────────────────┼─────────────────┼────────┘
           │ (Ephemeral Port)  │ (Ephemeral Port)│
           ▼                   ▼                 ▼
     Upstream DNS 1      Upstream DNS 2    Upstream DNS 3
      (8.8.8.8:53)       (10.0.0.1:53)      (1.1.1.1:53)
```

---

## Public Interfaces

### `DnsForwarder`

Inherits from `gh::ServiceBase`. Represents the DNS Forwarder service lifecycle.

```cpp
#include "DnsForwarder.hpp"

namespace gh::dns {

// Instantiate forwarder with executor and add listening endpoints
boost::asio::io_context ioContext;
auto executor = ioContext.get_executor();

DnsForwarder forwarder(executor);
co_await forwarder.AddListener(
  boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::loopback(), 53)
);

// Add upstreams by providing a list of upstream endpoints
auto publicRes = co_await forwarder.AddUpstream({
  boost::asio::ip::udp::endpoint(boost::asio::ip::address::from_string("8.8.8.8"), 53),
  boost::asio::ip::udp::endpoint(boost::asio::ip::address::from_string("8.8.4.4"), 53)
});
auto publicUpstream = publicRes.value();

auto internalRes = co_await forwarder.AddUpstream({
  boost::asio::ip::udp::endpoint(boost::asio::ip::address::from_string("10.0.0.1"), 53)
});
auto internalUpstream = internalRes.value();

// Configure domain routes using domain suffixes (matches domain and all subdomains)
forwarder.AddRoute("internal.company.com", internalUpstream); // Matches internal.company.com & all subdomains
forwarder.SetDefaultRoute(publicUpstream);                     // Sets default fallback upstream for unmatched queries

// Start the forwarder service
auto err = co_await forwarder.Start();
if (err) {
  // Handle startup error
}

// Stop the service gracefully
co_await forwarder.Stop();

} // namespace gh::dns
```

---

## Dynamic Runtime Configuration APIs

`DnsForwarder` provides thread-safe / fiber-safe APIs to manipulate listening endpoints, upstreams, and domain routing rules dynamically on the fly via RemoteCall without restarting the service.

### 0. Listening Endpoint Management

```cpp
// Add a listener dynamically inside DnsForwarder fiber
auto listenerRes = co_await forwarder.AddListener(boost::asio::ip::udp::endpoint(boost::asio::ip::address_v6::loopback(), 53));

// Remove a listener dynamically inside DnsForwarder fiber
co_await forwarder.RemoveListener(listenerRes.value());
```

### 1. Dynamic Upstream Management

```cpp
// Add a new upstream at runtime inside DnsForwarder fiber
auto secureRes = co_await forwarder.AddUpstream({
  boost::asio::ip::udp::endpoint(boost::asio::ip::address::from_string("9.9.9.9"), 53),
  boost::asio::ip::udp::endpoint(boost::asio::ip::address::from_string("149.112.112.112"), 53)
});

// Remove an upstream safely inside DnsForwarder fiber
co_await forwarder.RemoveUpstream(internalUpstream);
```

### 2. Dynamic Domain Route Management

```cpp
// Add or override a domain suffix routing rule on the fly
forwarder.AddRoute("github.com", secureRes.value()); // Matches github.com and all subdomains

// Remove a domain suffix routing rule
forwarder.RemoveRoute("github.com");

// Set default fallback route upstream for unmatched domain queries
forwarder.SetDefaultRoute(publicUpstream);
```

---

## Integration Guidelines

1. **Service Integration**: Instantiate `DnsForwarder` inside your service topology or application lifecycle.
2. **Ephemeral Port Allocation**: Sockets for outbound queries automatically request ephemeral local ports from the OS kernel.
3. **Dynamic Routing**: Use `AddUpstream` and `AddRoute` to dynamically construct or modify resolution paths on the fly.
