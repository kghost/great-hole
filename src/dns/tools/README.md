# Standalone DNS Forwarder Tool (`src/dns/tools`)

The `dns-forwarder` tool is a standalone daemon and executable in `src/dns/tools` that runs a GreatHole DNS forwarder service (`DnsForwarder`) powered by the `Omni::Fiber` asynchronous runtime.

---

## Key Features

1. **Standalone Service**: Runs independently without requiring the full GreatHole VPN stack.
2. **Native C++ Header Configuration**: Configured entirely via `DnsForwarderToolConfig.hpp`, avoiding runtime command-line argument parsing and providing compile-time type safety.
3. **Multi-Upstream & Domain Routing**: Supports routing queries by exact domain or domain suffix to dedicated upstream DNS servers, with fallback to a default upstream.
4. **Fixed Ephemeral Source Ports**: Each upstream binds its UDP socket to an OS-assigned ephemeral source port that remains constant throughout its lifecycle, enabling firewall and network-layer traffic classification.
5. **Structured Lifecycle & Graceful Teardown**: Gracefully traps `SIGINT` (Ctrl+C) and `SIGTERM` signals via `boost::asio::signal_set`, cleanly terminating all listening sockets, upstream sockets, and in-flight coroutine request fibers.

---

## Configuration (`DnsForwarderToolConfig.hpp`)

The forwarder configuration is defined in [DnsForwarderToolConfig.hpp](file:///home/kghost/workspace/great-hole/src/dns/tools/DnsForwarderToolConfig.hpp):

```cpp
#pragma once

#include "../../interface/InterfaceCommonTypes.hpp"

namespace gh::dns::tools {

inline auto GetDnsForwarderConfig() -> Interface::DnsForwarderConfiguration {
  return Interface::DnsForwarderConfiguration{
      // Inbound listening endpoints
      .Listeners = {
          {
              .LocalEndpoint =
                  Interface::DnsEndpoint{
                      .Address = Interface::Ip4Address{.Bytes = {127, 0, 0, 1}},
                      .Port = 53053,
                  },
          },
      },
      // Upstream DNS server pools
      .Upstreams = {
          {
              .Name = "public_dns",
              .ServerEndpoints = {
                  Interface::DnsEndpoint{.Address = Interface::Ip4Address{.Bytes = {8, 8, 8, 8}}, .Port = 53},
                  Interface::DnsEndpoint{.Address = Interface::Ip4Address{.Bytes = {1, 1, 1, 1}}, .Port = 53},
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
          {"internal.company.com", "corp_dns"},
      },
  };
}

} // namespace gh::dns::tools
```

---

## Building and Running

### Build

Use predefined CMake workflow presets:

```bash
# Debug build and test workflow
cmake --workflow --preset clang-debug
```

The compiled binaries will be generated at:
- `build-clang-debug/src/dns/tools/dns-forwarder`
- `build-clang-debug/src/dns/tools/dns-forwarder-asan`

### Execution

```bash
./build-clang-debug/src/dns/tools/dns-forwarder
```

Upon launch, the tool displays active listeners, upstream resolvers with their assigned ephemeral local ports, and routing policy:

```text
========================================
       GreatHole DNS Forwarder          
========================================
Inbound Listeners:
  - 127.0.0.1:53053

Upstream Resolvers:
  - [public_dns] Ephemeral Source Port: 44666
    Servers:
      * 8.8.8.8:53
      * 1.1.1.1:53
  - [corp_dns] Ephemeral Source Port: 34615
    Servers:
      * 10.0.0.1:53

Routing Policy:
  Default Route: public_dns
  Domain Suffix Rules:
    * internal.company.com -> corp_dns
========================================
[DnsForwarder] Service is active. Press Ctrl+C to stop.
```

### Stopping the Tool

Press `Ctrl+C` or send `SIGTERM`. The forwarder catches the signal, cleanly stops all subordinate services, joins child fibers, and terminates with exit code 0.
