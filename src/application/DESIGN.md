# GreatHole Application Module - Design & Architecture

This document describes the internal implementation details of the application module, with a focus on the `ConnectionTracker`.

## ConnectionTracker Architecture

`ConnectionTracker` maps network packets to `ConnectionMark` instances. This mapping is stored in specialized lookup tables for different protocols and IP versions.

### Data Structures

Connections are categorized by keys derived from their protocol and address details:
- **`Ip4TcpKey` / `Ip6TcpKey`**: Local & Remote IP address, Local & Remote Port.
- **`Ip4UdpKey` / `Ip6UdpKey`**: Local & Remote IP address, Local & Remote Port.
- **`IcmpKey` / `Icmp6Key`**: Local & Remote IP address, ICMP Identifier.

Each lookup table is structured as a `std::set` using a transparent comparator `EntryCompare`:
- `_Ip4TcpTable` / `_Ip6TcpTable`
- `_Ip4UdpTable` / `_Ip6UdpTable`
- `_IcmpTable` / `_Icmp6Table`

#### Transparent Comparators
`EntryCompare` allows looking up entries inside the `std::set` without creating complete entry objects, passing only the respective key structure.

---

### Lookup and Expiration Logic

To prevent memory leaks and handle dead connections, entries are timed out and pruned.

#### Background Pruner Loop
`DoWork()` runs a loop that periodically (every 5 seconds) prunes expired entries from all tables. Each entry tracks its `LastActive` time.

#### Idle Timeouts
- **TCP**:
  - `SynSent`: 60 seconds.
  - `Established`: 1200 seconds.
  - `FinWait`/`Closed`: 30 seconds.
- **UDP / ICMP**: 30 seconds.

##### TCP Bidirectional State Resolution
`TcpEntry` tracks state independently for `OutputDirection` and `InputDirection`:
- If either direction is closing or closed (`kFinSent`, `kFinAcked`, or `kClosed` via RST), the connection timeout resolves to `FinTimeout` (30s).
- Otherwise, if either direction is established (`kSynAcked`), the connection timeout resolves to `EstablishedTimeout` (1200s).
- Otherwise, the connection defaults to `SynTimeout` (60s).
- Receiving RST (`kRst`) in either direction transitions the state to `kClosed`.

#### Lookup-Time Validation
When `LookupAndUpdate` is called:
1. It looks up the key in the appropriate table.
2. If found, it checks if the entry has expired based on its protocol-specific timeout.
3. **Expired entries** are immediately erased from the table, and the packet is re-evaluated as a new connection (inserting it and querying the selector to choose new routing marks).
4. **Active entries** are updated (`LastActive` set to `now`, TCP flags updated) and handled based on the lookup direction:
   - **`Input` Direction (Incoming)**: The entry's `Result` is updated by querying the selector.
   - **`Output` Direction (Outgoing)**: If the result is invalid (as checked by the virtual `Validate()` method of the mark), the selector is queried to select a new routing mark. Otherwise, the existing mark is returned.
5. If key parsing fails (e.g. invalid packet) or lookup fails, `LookupAndUpdate` returns `std::unexpected` containing the error code.

---

### ICMP Association / Error Handling & Lambda-based Parsing

`ConnectionTracker` associates incoming ICMP error messages (such as Destination Unreachable) with their corresponding active UDP or TCP connections.

#### Lambda-based Key Parsing
`ParseConnectionKey` is a template method that accepts two direction template parameters (`KeyDirection` for key parsing, `ActionDirection` for action/return types) and a generic/auto callback `f`:
```cpp
template <typename KeyDirection, typename ActionDirection, typename F>
static std::expected<typename ActionDirection::ConnectionTrackerOutput, UnsupportedPacket>
ParseConnectionKey(std::span<const uint8_t> p, bool truncated, F&& f);
```
Rather than returning a variant (which requires runtime overhead and `std::visit` dispatching), it constructs the concrete key statically (e.g. `Ip4TcpKey`) and invokes the callback `f(key)` directly. This avoids variant wrapping/unwrapping and provides compile-time specialization. Returning `std::expected` allows propagating key-parsing failures (e.g. `UnsupportedPacket`) and recursion errors cleanly without having to match callback return types across recursive boundaries (such as ICMP Destination Unreachable payload recursion).

#### ICMP Key Parsing Workflow
1. `ParseConnectionKey` inspects the ICMP payload.
2. If the ICMP packet is a destination unreachable / error response containing the header of the original packet, the parser extracts the inner payload to resolve the connection key.
3. The lookup direction is inverted (using `KeyDirection::OppositeDirection`), allowing `LookupAndUpdate` to match the ICMP packet with the correct original session.
4. The parser recursively invokes itself with `KeyDirection::OppositeDirection` to process the inner packet.
5. The resolved key is passed to the lambda to execute table lookup, validation, and update.

---

## VpnClientMultiChannel & TunnelDataPlane Architecture

`TunnelDataPlane` constructs and owns a single `VpnClientMultiChannel` (`_Client`) instance alongside `ConnectionTracker` (`_ConnectionTracker`) for the entire lifetime of the `TunnelDataPlane` object. `VpnClientMultiChannel` manages multiple parallel VPN connection sessions (`_Sessions`) mapped to virtual network interfaces or specific remote endpoints.

### Multi-Run Service Lifecycle (`Start` / `Stop`)

- **Construction**: `VpnClientMultiChannel(executor, callbacks, tracker, selector)` initializes the persistent session registry (`_Sessions`) without requiring `tun`, `udpDynMux`, or `filters`.
- **`RunningContext` (`std::optional<RunningContext> _RunningContext`)**: Groups all objects whose lifecycle is bounded by a single `Start()` / `Stop()` run (`Tun`, `UdpDynMux`, `Filters`, `TunSide`, `TunPipeline`, and `ChannelCall`).
- **`Start(tun, udpDynMux, filters)`**: Emplaces `_RunningContext` with the per-run `Endpoint` (`WinDivert` or `Tun`), `UdpDynMux`, `Filter` list, a fresh `TunSideEndpoint`, and `ChannelCall` (`Omni::Fiber::RemoteCall`), and starts `_ConnectionTracker`, `Tun`, `UdpDynMux`, `TunSide`, and `TunPipeline`.
- **`DoGracefulStop()`**: Stops `TunPipeline`, stops all active channels in `_Sessions` via `StopChannel(session)` (transitioning every session back to `State::kNone`), stops `TunSide`, `UdpDynMux`, `Tun`, and `_ConnectionTracker`, closes `ChannelCall`, and resets `_RunningContext`. Crucially, `_Sessions` is **not** cleared on `Stop()`, so registered `VpnClientMultiChannelSession` instances and external `std::weak_ptr<VpnClientMultiChannelSession>` (`VpnEndpoint`) handles remain valid across `Start()` / `Stop()` cycles.

### Session Lifecycle & State Machine

Sessions are represented by `VpnClientMultiChannelSession`, which manages its state via an explicit `StateMachine` (`State::kNone`, `State::kStarting`, `State::kStartInterrupted`, `State::kRunning`, `State::kStopping`):

#### 1. Registration (`RegisterChannel`)
- Can be called at any time (before `Start()`, while running, or after `Stop()`).
- Creates a new `VpnClientMultiChannelSession` in state `State::kNone` and inserts the `std::shared_ptr` into `_Sessions` (a `std::set` using `std::owner_less`).

#### 2. Channel Start (`StartChannel`)
- Requires `VpnClientMultiChannel` to be in `State::kRunning`.
- Transitions session state `kNone` -> `kStarting` via `ActionStart` and notifies `DataPlaneCallbacks` via `OnEndpointStateChanged` (`Starting`).
- Creates a `UdpDynMux::Channel` targeting the remote endpoint. If channel creation fails, `OnEndpointStateChanged` (`Failed`) is emitted.

#### 3. Channel Establishment (`OnChannelEstablished`)
- When underlying channel negotiation completes, `OnChannelEstablished` transitions state `kStarting` -> `kRunning` via `ActionStarted`.
- Initializes `ChannelSideEndpoint` and `SessionPipeline`, then notifies `DataPlaneCallbacks` via `OnEndpointStateChanged` (`Running`).

#### 4. Unexpected Disconnect (`OnChannelClosed` when `kRunning`)
- If the network channel drops while in `kRunning`, `OnChannelClosed` tears down the pipeline and transitions state back to `kStarting` via `ActionStopped`, notifying `DataPlaneCallbacks` via `OnEndpointStateChanged` (`Starting`) to trigger auto-reconnection.

#### 5. Stop & Unregister (`StopChannel` / `UnregisterChannel`)
- `StopChannel` transitions state `kRunning` -> `kStopping` via `ActionStop` (or `kStarting` -> `kStartInterrupted` -> `kNone`), notifying `DataPlaneCallbacks` via `OnEndpointStateChanged` (`Stopping`).
- Erases the channel from `UdpDynMux`, triggering `OnChannelClosed` which transitions `kStopping` -> `kNone` via `ActionStopped` and notifies `DataPlaneCallbacks` via `OnEndpointStateChanged` (`Stopped`).
- `UnregisterChannel` requires state `kNone` and erases the session from `_Sessions` (can be called before `Start()`, while running after `StopChannel`, or after `Stop()`).

