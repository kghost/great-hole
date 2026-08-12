# DNS Forwarder Internal Design (`src/dns`)

This document describes the internal architecture, design decisions, component interactions, and state machines for the `DnsForwarder` module in `src/dns`.

---

## 1. Architectural Principles & Requirements

The `DnsForwarder` module is designed to fulfill the following core requirements:

1. **Standard DNS Server Inbound Interface**: Listen on configured UDP (and optionally TCP) local socket endpoints to serve standard DNS queries from OS resolvers or applications.
2. **Multi-Upstream Architecture**: Maintain isolated `DnsUpstream` instances. Each upstream operates with:
   - Dedicated list of upstream server endpoints (IP and port).
   - Dedicated **ephemeral local source port** bound UDP socket.
3. **Domain-Based Request Routing**: Route queries dynamically using a rule engine (`DnsRouter`) that evaluates QNAME against exact domains, domain suffix patterns (`example.com`), and default fallback rules.
4. **Fiber Native Execution**: Built on `OmniFiber` and `gh::ServiceBase` for non-blocking asynchronous I/O and structured fiber lifecycle management.

---

## 2. Component Architecture

```
                                ┌────────────────────────────────┐
                                │          DnsForwarder          │
                                │   (gh::ServiceBase subclass)   │
                                └───────────────┬────────────────┘
                                                │
                        ┌───────────────────────┴───────────────────────┐
                        ▼                                               ▼
              ┌───────────────────┐                           ┌───────────────────┐
              │   Server Listener │                           │     DnsRouter     │
              │   (UDP Socket)    │                           │ (Pattern Matcher) │
              └─────────┬─────────┘                           └─────────┬─────────┘
                        │                                               │
                        │ 1. Recv DNS Request                           │
                        ├───────────────────────────────────────────────┘
                        │ 2. Lookup Upstream for domain
                        ▼
             ┌─────────────────────┐
             │ DnsUpstream Selector│
             └──────────┬──────────┘
                        │
      ┌─────────────────┼─────────────────┐
      ▼                 ▼                 ▼
 ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
 │ DnsUpstream 1│ │ DnsUpstream 2│ │ DnsUpstream N│
 │ ┌──────────┐ │ │ ┌──────────┐ │ │ ┌──────────┐ │
 │ │Ephemeral │ │ │ │Ephemeral │ │ │ │Ephemeral │ │
 │ │ Socket   │ │ │ │ Socket   │ │ │ │ Socket   │ │
 │ └────┬─────┘ │ │ └────┬─────┘ │ │ └────┬─────┘ │
 └──────┼───────┘ └──────┼───────┘ └──────┼───────┘
        │ (Ephemeral)    │ (Ephemeral)    │ (Ephemeral)
        ▼                ▼                ▼
    Upstream 1       Upstream 2       Upstream N
```

---

## 3. Subsystem Detailed Specification

### 3.1 DNS Wire Parser (`DnsPacket`)

`DnsPacket` provides lightweight zero-copy parsing and serialization for DNS wire messages.

- **Header Structure**:
  - `id`: 16-bit Transaction ID.
  - `flags`: QR, Opcode, AA, TC, RD, RA, Z, RCODE.
  - `qdcount`, `ancount`, `nscount`, `arcount`: 16-bit record counts.
- **Section Parsing**:
  - `Question`: Domain QNAME (compressed label decoding), QTYPE (A, AAAA, CNAME, PTR, MX, TXT, etc.), QCLASS (IN).
  - `ResourceRecord`: Name, Type, Class, TTL (32-bit uint), RDATA (RDLENGTH + payload).
- **Label Decompression**: Handles pointer offsets (`0xC000` mask) safely with recursion/loop limit checks to prevent malformed packet loops.

### 3.2 Routing Engine (`DnsRouter`)

`DnsRouter` resolves domain names to `DnsUpstream` instances (`std::weak_ptr<DnsUpstream>`).

- **Rule Representation**:
  ```cpp
  struct DnsRouteRule {
    std::string domainSuffix;        // e.g. "google.com", "internal.company.com", "" (empty = default fallback)
    std::weak_ptr<DnsUpstream> upstream; // Target upstream weak pointer
  };
  ```
- **Matching Algorithm**:
  1. Domain Suffix match: Query domain `sub.internal.company.com` matches suffix `internal.company.com` or exact domain `internal.company.com`. Longest matching suffix wins (most specific rule).
  2. Default fallback rule: Empty domain suffix `""` matches all domains that have no specific suffix rule.

### 3.3 Upstream Client (`DnsUpstream`)

Each `DnsUpstream` encapsulates the resolution loop for its assigned route partition across a **list of upstream servers** (e.g. primary and secondary DNS servers).

- **Multi-Upstream Server List**:
  - Configured with `std::vector<boost::asio::ip::udp::endpoint> upstreamServers`.
  - Queries can be sent to the primary upstream server with transparent fallback / retry to secondary upstreams if a timeout occurs, or load-balanced across the list.
- **Ephemeral Port Socket Fixed for Upstream Lifetime**:
  - Opens a `boost::asio::ip::udp::socket` bound to local port `0` (`0.0.0.0:0` or `127.0.0.1:0`) at construction time.
  - The OS kernel assigns an ephemeral local port (e.g. `51234`) which remains **fixed for the entire lifetime of that `DnsUpstream` instance**.
  - All outbound queries sent by this `DnsUpstream` share this exact bound socket and local port, making outbound packets consistently identifiable to network-layer rules (WinDivert, TUN, iptables).
  - Exposes `uint16_t GetLocalPort() const` to retrieve the bound local port.
- **Transaction Multiplexing**:
  - Maintains an active transaction map: `std::unordered_map<uint16_t, PendingQuery>` mapping DNS 16-bit Transaction IDs to waiting fiber coroutines.
  - Outbound queries assign a unique or managed transaction ID to prevent collisions.
- **Query Resolution Workflow**:
  ```
  Client Request -> DnsUpstream::Resolve()
        │
        ▼
  Send Upstream UDP Packet (from ephemeral local port to Upstream Server List)
        │
  Await Upstream Response (via PendingQuery map)
        │
        ▼
  Return Response to Forwarder
  ```

### 3.4 Dynamic Reconfiguration Architecture

To support runtime tweaks without interrupting DNS forwarding, `DnsForwarder` implements concurrent, thread-safe & fiber-safe dynamic mutation mechanisms:

- **Simplified Upstream Management (`AddUpstream` / `RemoveUpstream`)**:
  - `AddUpstream(upstreamServers)` creates a `DnsUpstream` using ephemeral local ports, registers it internally, and returns a `std::shared_ptr<DnsUpstream>`.
  - `RemoveUpstream(upstream)` unregisters the upstream, redirects associated routes to the default upstream, and gracefully closes the upstream's socket inside `DnsForwarder`'s fiber context.
- **Dynamic Routing Table (`DnsRouter`)**:
  - `AddRoute(domainSuffix, upstream)`, `RemoveRoute(domainSuffix)`, and `SetDefaultRoute(upstream)` update domain mappings.

---

## 4. Concurrency & Lifetime Management

- **ServiceBase Integration**: `DnsForwarder` subclasses `gh::ServiceBase`.
  - `DoStart()`: Binds inbound DNS server sockets across all configured listening `endpoints`.
  - `DoWork()`: Spawns inbound server UDP listener fiber loops across `endpoints` and background transaction loops.
  - `DoGracefulStop()`: Closes server listener sockets, cancels pending query fibers, and stops child upstream sockets.
- **Fiber Safety**: Sockets and transaction tables are bound to the fiber execution context. Thread-safety or fiber synchronization primitives (`omni::fiber::Event`, mutexes) ensure zero race conditions under concurrent fiber operations.

---

## 5. File Structure in `src/dns`

```
src/dns/
├── CMakeLists.txt         # CMake build configuration for gh_dns library
├── README.md              # Public API and usage guide
├── DESIGN.md              # Internal design specifications (this document)
├── DnsPacket.hpp          # DNS header and wire-format packet parser/builder
├── DnsPacket.cpp
├── DnsRouter.hpp          # Domain to client routing table
├── DnsRouter.cpp
├── DnsUpstream.hpp        # Upstream client with ephemeral local port socket
├── DnsUpstream.cpp
├── DnsListener.hpp        # Inbound UDP listener service (ServiceBase)
├── DnsListener.cpp
├── DnsForwarder.hpp       # Main forwarder service (ServiceBase)
├── DnsForwarder.cpp
└── tests/                 # Unit tests directory
    └── DnsForwarderTest.cpp
```
