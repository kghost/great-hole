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
