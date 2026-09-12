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
│  └───────┬───────────────────┬──────────────────┬────┘  │
│          │ google.com        │ internal         │ default
│          ▼                   ▼                  ▼       │
│  ┌───────────────┐   ┌───────────────┐   ┌────────────┐ │
│  │ DnsUpstream 1 │   │ DnsUpstream 2 │   │DnsUpstream3│ │
│  └───────┬───────┘   └───────┬───────┘   └──────┬─────┘ │
└──────────┼───────────────────┼──────────────────┼───────┘
           ▼                   ▼                  ▼
     Upstream DNS 1      Upstream DNS 2    Upstream DNS 3
      (8.8.8.8:53)       (10.0.0.1:53)      (1.1.1.1:53)
```

---

## Public Interfaces

### `DnsForwarderCallbacks`

A pure virtual interface for receiving notifications about inbound **A** and **AAAA** DNS queries and their resolution results.

```cpp
#include "InterfaceCommonTypes.hpp"

namespace gh::Interface {

using DnsQueryResult = std::variant<std::span<const Ip4Address>, std::span<const Ip6Address>>;

class DnsForwarderCallbacks {
public:
  explicit DnsForwarderCallbacks() = default;
  virtual ~DnsForwarderCallbacks() = default;

  virtual void OnDnsQueryResult(
      const std::string& upstream,
      const std::string& domain,
      DnsQueryResult results
  ) = 0;
};

} // namespace gh::Interface
```

### `DnsForwarder`

Inherits from `gh::ServiceBase`. Represents the DNS Forwarder service lifecycle. `DnsForwarder` is configured once upon construction via `gh::Interface::DnsForwarderConfiguration` and a reference to `DnsForwarderCallbacks`, remaining immutable throughout its runtime lifecycle.

```cpp
#include "DnsForwarder.hpp"

namespace gh::dns {

boost::asio::io_context ioContext;
auto executor = ioContext.get_executor();

// 1. Define declarative configuration
Interface::DnsForwarderConfiguration config{
  // Inbound listening endpoints
  .Listeners = {
    {.LocalEndpoint = Interface::DnsEndpoint{
       .Address = Interface::Ip4Address{.Bytes = {127, 0, 0, 1}},
       .Port = 53,
    }},
  },
  // Upstream DNS servers
  .Upstreams = {
    {
      .Name = "public_dns",
      .ServerEndpoints = {
        Interface::DnsEndpoint{.Address = Interface::Ip4Address{.Bytes = {8, 8, 8, 8}}, .Port = 53},
        Interface::DnsEndpoint{.Address = Interface::Ip4Address{.Bytes = {8, 8, 4, 4}}, .Port = 53},
      },
    },
    {
      .Name = "corp_dns",
      .ServerEndpoints = {
        Interface::DnsEndpoint{.Address = Interface::Ip4Address{.Bytes = {10, 0, 0, 1}}, .Port = 53},
      },
    },
  },
  // Default fallback upstream name
  .DefaultRoute = "public_dns",
  // Domain suffix routing rules (domain -> upstream name)
  .Routes = {
    {"internal.company.com", "corp_dns"}, // Matches internal.company.com & all subdomains
  },
};

// 2. Implement callback interface
class MyDnsCallbacks : public DnsForwarderCallbacks {
public:
  void OnDnsQueryResult(const std::string& upstream, const std::string& domain,
                        DnsQueryResult results) override {
    // Process A / AAAA resolution results
  }
};
MyDnsCallbacks callbacks;

// 3. Instantiate forwarder with executor, configuration, and callbacks
DnsForwarder forwarder(executor, std::move(config), callbacks);

// 4. Start the forwarder service (starts all upstreams, router, and listeners)
auto err = co_await forwarder.Start();
if (err) {
  // Handle startup error
}

// 5. Query runtime configuration snapshot (includes allocated ephemeral ports)
auto runtimeConfig = forwarder.GetConfiguration();
for (const auto& upstream : runtimeConfig.Upstreams) {
  // upstream.LocalPort contains the OS-assigned ephemeral source port
}

// 6. Stop the service gracefully
co_await forwarder.Stop();

} // namespace gh::dns
```

---

## Runtime Configuration Inspection

`DnsForwarder` exposes its complete configuration and active state via `GetConfiguration()`, returning `gh::Interface::DnsForwarderConfiguration`:

```cpp
auto config = forwarder.GetConfiguration();

// 1. Inbound listener local endpoints
for (const auto& listener : config.Listeners) {
  // listener.LocalEndpoint: gh::Interface::DnsEndpoint { Address, Port }
}

// 2. Upstreams, remote server endpoints, and ephemeral local ports
for (const auto& upstream : config.Upstreams) {
  // upstream.Name: std::string
  // upstream.ServerEndpoints: std::vector<gh::Interface::DnsEndpoint>
  // upstream.LocalPort: uint16_t (bound ephemeral UDP source port)
}

// 3. Default route and domain routing rules
if (config.DefaultRoute.has_value()) {
  // *config.DefaultRoute: std::string (upstream name)
}
for (const auto& [domainSuffix, upstreamName] : config.Routes) {
  // domainSuffix: std::string, upstreamName: std::string
}
```

---

## Integration Guidelines

1. **Service Integration**: Instantiate `DnsForwarder` inside your service topology or application lifecycle, passing `DnsForwarderConfiguration` at construction.
2. **Ephemeral Port Allocation**: Sockets for outbound queries automatically request ephemeral local ports from the OS kernel, inspectable via `GetConfiguration()`.
3. **Immutable Lifecycle**: Routing rules, listening endpoints, and upstreams are established deterministically on construction for race-free concurrency.

---

## Standalone DNS Forwarder Tool

A standalone daemon executable (`dns-forwarder`) configured via native C++ header is available in `src/dns/tools`. See [src/dns/tools/README.md](tools/README.md) for usage, configuration, and build instructions.

