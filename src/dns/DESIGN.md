# DNS Forwarder Internal Design (`src/dns`)

This document describes the internal architecture, design decisions, component interactions, concurrency model, and state machines for the `DnsForwarder` module in `src/dns`.

---

## 1. Architectural Principles & Requirements

The `DnsForwarder` module provides a multi-upstream DNS forwarding service designed around the following architectural principles:

1. **Standard Inbound DNS Interface**:
   - Listens on configurable local UDP endpoints (e.g. `127.0.0.1:53` or `[::1]:53`) via dedicated listener services (`DnsListener`).
   - Parses RFC 1035 wire-format DNS packets, routes queries based on the requested domain name (QNAME), and replies with standard DNS response datagrams.

2. **Isolated Multi-Upstream Architecture**:
   - Maintains isolated `DnsUpstream` service instances.
   - Each upstream operates with a configured list of target upstream server endpoints (e.g. primary and secondary DNS resolvers) with automatic failover.
   - Each upstream binds to an **ephemeral local source port** on construction that remains **fixed throughout the lifetime of the upstream instance**. This ensures consistent network-level identification (e.g., for WinDivert packet filtering, TUN interfaces, or firewall policies).

3. **Domain-Based Request Routing**:
   - Evaluates incoming queries using `DnsRouter`.
   - Supports exact domain matching, longest-matching domain suffixes (subdomains), and a default fallback upstream.
   - Automatically handles weak reference tracking to ensure clean lifecycle separation between router rules and upstream objects.

4. **OmniFiber Concurrency & Structured Concurrency**:
   - Built on `OmniFiber` coroutines and `gh::ServiceBase`.
   - Leverages `Omni::Fiber::RemoteCall` for serialized, fiber-safe dynamic mutations without lock contention.
   - Concurrently processes incoming client queries by spawning managed child fibers per request.

---

## 2. Component Architecture & End-to-End Query Flow

### 2.1 Component Topology

```
                                  ┌───────────────────────────────┐
                                  │         DnsForwarder          │
                                  │   (gh::ServiceBase composite) │
                                  │   RPC: _ClientRpc             │
                                  └───────────────┬───────────────┘
                                                  │
                ┌─────────────────────────────────┼─────────────────────────────────┐
                ▼                                 ▼                                 ▼
      ┌───────────────────┐             ┌───────────────────┐             ┌───────────────────┐
      │   DnsListener 1   │             │     DnsRouter     │             │    DnsUpstream    │
      │    (UDP:53053)    │             │ (Pattern Matcher) │             │ (LocalPort:52134) │
      │ ┌───────────────┐ │             │ RPC: _Rpc         │             │ ┌───────────────┐ │
      │ │  async_rx     │ │             └─────────┬─────────┘             │ │  async_rx     │ │
      │ └───────┬───────┘ │                       │                       │ └───────┬───────┘ │
      └─────────┼─────────┘                       │                       └─────────┼─────────┘
                │                                 │                                 │
                │ 1. Recv packet                  │                                 │
                ├────────────────────────────────►│                                 │
                │    HandleRequest(...)           │                                 │
                │                                 │ 2. Spawn DnsRequest-<id>        │
                │                                 │    Route(QNAME) -> Upstream     │
                │                                 │ 3. Resolve(pkt, cancel)         │
                │                                 ├────────────────────────────────►│
                │                                 │                                 │ 4. async_send_to
                │                                 │                                 │    Await Event
                │                                 │                                 │ 5. async_rx
                │                                 │                                 │    Fire Event
                │                                 │ 6. Return response packet       │
                │                                 │◄────────────────────────────────┘
                │ 7. SendResponse(...)            │
                │◄────────────────────────────────┘
                │
                ▼
        [ DNS Client Socket ]
```

### 2.2 End-to-End Resolution Lifecycle

1. **Client Query Ingestion**:
   - A client sends a DNS query datagram to a local listening endpoint.
   - `DnsListener::DoWork()` receives the datagram via `_Socket.async_receive_from()`.
   - The raw byte buffer and sender endpoint are forwarded to `DnsRouter::HandleRequest(*this, senderEp, std::move(rxBuffer))`.

2. **Request Scheduling & Dispatch**:
   - `DnsRouter::HandleRequest()` calls `_Rpc.Call()`, queueing the request into the router's fiber event loop.
   - The router creates a `Cancel` token and spawns a dedicated child fiber: `currentFiber.Spawn("DnsRequest-" + std::to_string(reqId), ...)`.
   - The child fiber handle and cancellation token are stored in `_RequestFibers`.

3. **Packet Parsing & Routing**:
   - The child fiber parses the raw datagram using `DnsPacket::Parse()`. If parsing fails, the packet is dropped.
   - The primary QNAME is extracted via `parseResult->GetPrimaryQName()`.
   - `DnsRouter::Route(qname)` performs normalized domain suffix matching against the routing table (`_Routes`), falling back to `_DefaultRoute` if no specific suffix matches.

4. **Upstream Forwarding & Transaction Multiplexing**:
   - If an upstream is found, the child fiber executes `co_await routeTarget->Resolve(std::move(*parseResult), *cancelToken)`.
   - `DnsUpstream::Resolve()` generates a unique client transaction ID via `GenerateTxId()`, creates an `Omni::Fiber::Event`, stores it in `_PendingQueries`, rewrites the packet header ID, serializes the packet, and transmits it via `_Socket.async_send_to()` to target upstream servers sequentially until one succeeds.
   - The coroutine suspends awaiting the upstream `Event`.

5. **Upstream Ingestion & Demultiplexing**:
   - When the remote DNS server replies, `DnsUpstream::ReceiveLoop()` receives the datagram via `_Socket.async_receive_from()`.
   - It parses the response using `DnsPacket::Parse()`, reads the transaction ID, locates the matching `PendingQuery` entry in `_PendingQueries`, rewrites the transaction ID back to the client's original ID, and fires the `Event`.
   - Suspended query fibers wake up with the resolution result.

6. **Error Synthesis & Response Delivery**:
   - If upstream resolution fails, `DnsRouter` generates a synthetic `DnsRCode::ServFail` error response using `DnsPacket::MakeErrorResponse(queryId, ServFail)`.
   - If no route matches the domain, `DnsRouter` generates a synthetic `DnsRCode::Refused` error response.
   - The child fiber invokes `co_await listener.SendResponse(sender, std::move(txBuffer))` which transmits the response to the client via `_Socket.async_send_to()`.
   - When the child fiber exits, `DnsRouter::DoWork()` reaps completed fiber handles using `currentFiber.TryWait()`.

---

## 3. Subsystem Detailed Specifications

### 3.1 DNS Wire Parser & Serializer (`DnsPacket`)

`DnsPacket` provides lightweight, zero-copy parsing (over `std::span<const uint8_t>`) and serialization for standard DNS wire messages conforming to RFC 1035.

#### Header Structure & Flags (`DnsHeader`)
- **Fixed Size**: Exactly 12 bytes (`kDnsHeaderSize = 12`).
- **Fields**:
  - `Id` (16-bit): Transaction Identifier.
  - `Flags` (16-bit): Bitfield encoding QR, Opcode, AA, TC, RD, RA, Z, and RCODE.
  - `QdCount` (16-bit): Number of entries in Question section.
  - `AnCount` (16-bit): Number of Resource Records in Answer section.
  - `NsCount` (16-bit): Number of Name Server records in Authority section.
  - `ArCount` (16-bit): Number of Resource Records in Additional records section.
- **Bitwise Helpers**:
  - `kDnsFlagQrMask = 0x8000`: Bit 15 determines Query (0) vs Response (1).
  - `kDnsFlagRCodeMask = 0x000F`: Lower 4 bits determine Response Code (`DnsRCode`).
  - `kDnsFlagRCodeClearMask = 0xFFF0`: Mask for clearing RCODE prior to updating.
  - `IsQuery()`, `IsResponse()`, `SetResponse(bool)`.
  - `GetRCode()`, `SetRCode(DnsRCode)`.

#### Supported Enums
- **`DnsType`**:
  - `A = 1`, `NS = 2`, `CNAME = 5`, `SOA = 6`, `PTR = 12`, `MX = 15`, `TXT = 16`, `AAAA = 28`, `SRV = 33`, `ANY = 255`.
- **`DnsRCode`**:
  - `NoError = 0`: Successful resolution.
  - `FormErr = 1`: Format error in request.
  - `ServFail = 2`: Server failed to complete resolution.
  - `NXDomain = 3`: Non-existent domain.
  - `NotImp = 4`: Query type/operation not implemented.
  - `Refused = 5`: Query rejected due to policy / no matching route.

#### Record Representations
- **`DnsQuestion`**: `QName` (`std::string`), `QType` (`uint16_t`, default `A`), `QClass` (`uint16_t`, default `1` / `IN`).
- **`DnsResourceRecord`**: `Name` (`std::string`), `Type` (`uint16_t`), `RClass` (`uint16_t`), `Ttl` (`uint32_t`, default `300`), `RData` (`std::vector<uint8_t>`).

#### Label Compression & Parsing Security
- **Domain Decompression (`ParseDomainName`)**:
  - Handles pointer compression offsets indicated by `(len & 0xC0) == 0xC0`.
  - Enforces `kMaxPointerDepth = 16` recursion/jump limit. Exceeding this limit returns `SysError(ELOOP)` to prevent infinite recursion on malformed cyclic packets.
  - Validates buffer bounds on every pointer offset and label length, returning `SysError(EINVAL)` on truncations.
  - Tracks `nextOffset` before the initial pointer jump so parsing resumes at the correct byte offset after reading a compressed domain name.
- **Domain Serialization (`EncodeDomainName`)**:
  - Converts dot-delimited strings into length-prefixed DNS labels ending with a null byte (`0`).
  - Empty domains or `"."` encode directly to a single `0` byte.
  - Validates RFC label limits (`len <= 63`).
- **Error Response Generator (`MakeErrorResponse`)**:
  - `DnsPacket::MakeErrorResponse(uint16_t queryId, DnsRCode rcode)` creates a minimalist response packet with the specified transaction ID, QR flag set to 1, and the target RCODE.

---

### 3.2 Inbound Listener Service (`DnsListener`)

`DnsListener` is a `gh::ServiceBase` subclass responsible for managing a single local listening socket.

- **Socket Binding (`DoStart`)**:
  - Binds a `boost::asio::ip::udp::socket` to the specified `boost::asio::ip::udp::endpoint` (IPv4 or IPv6).
  - Configures socket reuse and opens according to the endpoint protocol.
- **Inbound Ingestion Loop (`DoWork`)**:
  - Allocates a 2048-byte receive buffer.
  - Calls `_Socket.async_receive_from(..., _Stop.AsioSlot()())`.
  - Resizes the buffer to match `bytesRecv` and dispatches to `_Router.HandleRequest(*this, senderEp, std::move(rxBuffer))`.
- **Client Response Transmission (`SendResponse`)**:
  - Sends outgoing serialized DNS responses directly to the client's UDP endpoint via `_Socket.async_send_to(boost::asio::buffer(data), sender, Omni::Fiber::AsioUseFiber)`.
- **Graceful Shutdown (`DoGracefulStop`)**:
  - Closes `_Socket`, canceling pending asynchronous receive operations.

---

### 3.3 Routing & Request Execution Engine (`DnsRouter`)

`DnsRouter` is a `gh::ServiceBase` subclass that coordinates domain matching, request queueing, and concurrent query fiber execution.

- **Domain Normalization (`NormalizeDomain`)**:
  - Converts all characters to lowercase (`std::tolower`).
  - Strips trailing dots (e.g., `"example.com."` -> `"example.com"`).
- **Routing Table Storage**:
  - `_Routes`: `std::unordered_map<std::string, std::weak_ptr<DnsUpstream>>`.
  - `_DefaultRoute`: `std::optional<std::weak_ptr<DnsUpstream>>`.
  - Using `std::weak_ptr` prevents reference loops and dangling references when upstreams are destroyed.
- **Route Resolution Algorithm (`Route`)**:
  1. Normalizes the query domain.
  2. If the domain is empty, attempts to lock and return `_DefaultRoute`.
  3. Checks `_Routes` for an exact normalized match. If present and lock succeeds, returns the upstream.
  4. Suffix walk: Scans dots from left to right (`norm.find('.', dotPos)`), extracting suffixes (e.g., for `a.b.c.com`, tests `b.c.com`, then `c.com`, then `com`). The leftmost dot corresponds to the longest matching suffix, guaranteeing that the most specific rule takes precedence.
  5. Fallback: If no suffix rule matches or matched upstreams have expired, attempts to lock and return `_DefaultRoute`.
  6. Returns `std::nullopt` if no valid upstream is found.
- **Concurrent Request Pipeline (`HandleRequest`)**:
  - Uses `Omni::Fiber::RemoteCall _Rpc` to safely submit requests into the router's fiber event loop.
  - Spawns a child fiber named `"DnsRequest-" + std::to_string(reqId)`.
  - Registers the child fiber in `_RequestFibers` alongside a dedicated `Cancel` token.
  - The child fiber executes:
    1. `DnsPacket::Parse()`: Drops packet if malformed.
    2. Extracts QNAME via `parseResult->GetPrimaryQName()`.
    3. Queries `Route(qname)`:
       - On match: calls `routeTarget->Resolve(std::move(*parseResult), *cancelToken)`.
         - On success: serializes answer packet.
         - On failure: logs error and generates `ServFail` response.
       - On no match: logs error and generates `Refused` response.
    4. If not canceled, calls `co_await listener.SendResponse(sender, std::move(txBuffer))`.
- **Fiber Event Loop & Child Fiber Reaping (`DoWork`)**:
  - Uses `Omni::Fiber::Select` to monitor three events:
    - Stop signal: `_Service.value()._Stop.GetFiberCancelEvent()`.
    - RPC requests: `_Rpc.GetServiceAwaitor()`.
    - Child fiber termination: `currentFiber.ChildAwaitor()`.
  - Periodically cleans up completed child fibers using `currentFiber.TryWait()` and `std::erase_if(_RequestFibers, ...)`.
  - On shutdown, triggers all `CancelToken`s and waits for all child fibers to finish via `currentFiber.WaitAll()`.
- **Graceful Shutdown (`DoGracefulStop`)**:
  - Discards and closes `_Rpc`.
  - Triggers all cancellation tokens in `_RequestFibers`.
  - Joins all remaining child fibers sequentially via `currentFiber.Join(ctx.FiberHandle)`.

---

### 3.4 Upstream Client (`DnsUpstream`)

`DnsUpstream` is a `gh::ServiceBase` subclass that encapsulates outbound communication with target upstream DNS servers.

- **Fixed Ephemeral Source Port Socket**:
  - `DoStart()` opens an IPv4 UDP socket (`boost::asio::ip::udp::v4()`) and binds to local port 0 (`0.0.0.0:0`).
  - Reads the OS-assigned ephemeral port: `_LocalPort = _Socket.local_endpoint().port()`.
  - The socket remains open with this exact local port for the entire lifetime of the `DnsUpstream` instance.
  - Exposes `GetLocalPort() const -> uint16_t` for external inspection and packet-filtering rules.
- **Transaction Multiplexing**:
  - `_PendingQueries`: Hash map `std::unordered_map<uint16_t, PendingQuery>`:
    ```cpp
    struct PendingQuery {
      uint16_t OriginalTxId{0};
      std::shared_ptr<Omni::Fiber::Event<std::expected<DnsPacket, ErrorCode>>> Event;
    };
    ```
  - `_NextTxId`: Monotonically increasing 16-bit counter (initialized to 1, skips 0 on overflow).
  - `GenerateTxId()`: Allocates collision-free transaction IDs for outbound upstream datagrams.
- **Outbound Resolution (`Resolve`)**:
  - Verifies service state is `kRunning` and `_UpstreamServers` is non-empty (returns `SysError(ENETUNREACH)` otherwise).
  - Stores `OriginalTxId` and an `Event` in `_PendingQueries[clientTxId]`.
  - Replaces `request.Header.Id` with `clientTxId` and serializes the packet.
  - Iterates through `_UpstreamServers`, calling `_Socket.async_send_to(..., cancel.AsioSlot()())`. Breaks on first successful send.
  - If all target servers fail, removes the pending entry and returns `SysError(EHOSTUNREACH)`.
  - Suspends on `co_await *event`.
  - Cleans up `_PendingQueries[clientTxId]` and returns the result.
- **Inbound Response Demultiplexing (`ReceiveLoop`)**:
  - Receives datagrams in a 2048-byte buffer via `_Socket.async_receive_from(..., _Stop.AsioSlot()())`.
  - Parses packet with `DnsPacket::Parse()`.
  - Looks up `parseResult->Header.Id` in `_PendingQueries`.
  - If matched, restores `parseResult->Header.Id = origTxId`, erases the entry from `_PendingQueries`, and fires `eventToNotify->Fire(std::move(*parseResult))`.
- **Graceful Shutdown (`DoGracefulStop`)**:
  - Closes `_Socket`.
  - Iterates through `_PendingQueries` and fires each pending `Event` with `SysError(ECANCELED)`.
  - Clears `_PendingQueries`.

---

### 3.5 Composite Service & Dynamic Reconfiguration (`DnsForwarder`)

`DnsForwarder` is the top-level composite `gh::ServiceBase` subclass that manages all subordinate components.

- **Component Ownership**:
  - `_Router`: `std::shared_ptr<DnsRouter>`.
  - `_Listeners`: `std::set<std::shared_ptr<DnsListener>, std::owner_less<>>`.
  - `_Upstreams`: `std::set<std::shared_ptr<DnsUpstream>, std::owner_less<>>`.
  - `_ClientRpc`: `Omni::Fiber::RemoteCall` for queueing dynamic administrative operations into the forwarder's fiber.
- **Dynamic Listener Management**:
  - `AddListener(endpoint)`: Creates a `DnsListener`, schedules registration via `_ClientRpc`, starts the listener asynchronously (`co_await listener->Start()`), and returns `std::weak_ptr<DnsListener>`.
  - `RemoveListener(weak)`: Locks weak pointer, schedules removal via `_ClientRpc`, stops the listener asynchronously (`co_await listener->Stop()`), and erases it from `_Listeners`.
- **Dynamic Upstream Management**:
  - `AddUpstream(upstreamServers)`: Creates a `DnsUpstream`, schedules registration via `_ClientRpc`, starts the upstream asynchronously (`co_await upstream->Start()`), and returns `std::weak_ptr<DnsUpstream>`.
  - `RemoveUpstream(weak)`: Locks weak pointer, schedules removal via `_ClientRpc`, stops the upstream asynchronously (`co_await upstream->Stop()`), and erases it from `_Upstreams`.
- **Dynamic Routing Rule Management**:
  - `AddRoute(domainSuffix, upstream)`: Delegates to `_Router->AddRoute(...)`.
  - `RemoveRoute(domainSuffix)`: Delegates to `_Router->RemoveRoute(...)`.
  - `SetDefaultRoute(upstream)`: Delegates to `_Router->SetDefaultRoute(...)`.
- **Service Lifecycle Coordination**:
  - `DoStart()`:
    1. Starts all registered `DnsUpstream` instances.
    2. Starts `_Router`.
    3. Starts all registered `DnsListener` instances.
  - `DoWork()`:
    - Waits via `Omni::Fiber::Select` on `_Stop.GetFiberCancelEvent()` and `_ClientRpc.GetServiceAwaitor()`, executing dynamic mutation requests.
  - `DoGracefulStop()`:
    1. Closes and flushes `_ClientRpc`.
    2. Stops and clears all `DnsListener` instances.
    3. Stops and clears all `DnsUpstream` instances.
    4. Stops `_Router`.

---

## 4. Concurrency Model & Structured Lifecycle

```
DnsForwarder Fiber (ServiceBase)
├── _ClientRpc Queue (Add/Remove Listeners & Upstreams)
├── DnsRouter Fiber (ServiceBase)
│   ├── _Rpc Queue (Inbound Requests)
│   └── Child Request Fibers [DnsRequest-1, DnsRequest-2, ...]
│       ├── Parse Packet
│       ├── Route & Resolve
│       └── Send Response to Listener
├── DnsListener Fibers (ServiceBase per Endpoint)
│   └── async_receive_from loop -> dispatches to Router
└── DnsUpstream Fibers (ServiceBase per Upstream)
    └── async_receive_from loop -> demuxes to PendingQuery Events
```

1. **Lock-Free Concurrency via Coroutines & RPC**:
   - Rather than relying on traditional mutexes, cross-service interactions and dynamic mutations pass through `Omni::Fiber::RemoteCall` message queues.
   - Dynamic listener and upstream mutations execute strictly within `DnsForwarder`'s fiber context.
   - Inbound DNS query processing is dispatched through `DnsRouter`'s `_Rpc` and executed concurrently across isolated child coroutine fibers.

2. **Transaction Demultiplexing via Fiber Events**:
   - `Omni::Fiber::Event<std::expected<DnsPacket, ErrorCode>>` acts as a one-shot async channel between the upstream `ReceiveLoop` and waiting `Resolve` coroutines.
   - Avoids thread contention and ensures zero-overhead fiber resumption.

3. **Cascading Cancellation & Graceful Teardown**:
   - All asynchronous I/O calls register cancellation slots (`cancel.AsioSlot()()` or `_Stop.AsioSlot()()`).
   - When `DnsForwarder::Stop()` is invoked:
     - Listeners close sockets immediately, stopping new inbound traffic.
     - Router cancels all in-flight request fibers and joins them.
     - Upstreams close sockets and fire `ECANCELED` on all pending query events, waking up any remaining fibers immediately.
     - Zero socket leaks or stranded fiber coroutines.

---

## 5. File Structure in `src/dns`

```
src/dns/
├── CMakeLists.txt         # CMake build configuration for gh_dns library
├── README.md              # Public API, configuration, and external usage guide
├── DESIGN.md              # Internal design specifications (this document)
├── DnsPacket.hpp          # DNS header, types, and wire-format packet parser/builder
├── DnsPacket.cpp
├── DnsRouter.hpp          # Domain normalization, routing table, and request dispatcher
├── DnsRouter.cpp
├── DnsListener.hpp        # Inbound UDP socket listener service (ServiceBase)
├── DnsListener.cpp
├── DnsUpstream.hpp        # Upstream client service with fixed ephemeral port (ServiceBase)
├── DnsUpstream.cpp
├── DnsForwarder.hpp       # Composite DNS forwarder service manager (ServiceBase)
├── DnsForwarder.cpp
└── tests/                 # Unit and integration test suite
    ├── CMakeLists.txt
    └── DnsForwarderTest.cpp
```
