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

#### Lookup-Time Validation
When `LookupAndUpdate` is called:
1. It looks up the key in the appropriate table.
2. If found, it checks if the entry has expired based on its protocol-specific timeout.
3. Expired entries are removed immediately.
4. Active entries are updated (`LastActive` set to `now`, TCP flags updated) and validated. If they fail validation, they are erased and `std::nullopt` is returned.

---

### ICMP Association / Error Handling & Lambda-based Parsing

`ConnectionTracker` associates incoming ICMP error messages (such as Destination Unreachable) with their corresponding active UDP or TCP connections.

#### Lambda-based Key Parsing
`ParseConnectionKey` is a template method that accepts a generic/auto lambda:
```cpp
template <typename F>
static bool ParseConnectionKey(const Packet& p, ConnectionDirection direction, F&& f);
```
Rather than returning a variant (which requires runtime overhead and `std::visit` dispatching), it constructs the concrete key statically (e.g. `Ip4TcpKey`) and invokes the callback `f(key)` directly. This avoids variant wrapping/unwrapping and provides compile-time specialization.

#### ICMP Key Parsing Workflow
1. `ParseConnectionKey` inspects the ICMP payload.
2. If the ICMP packet is a destination unreachable / error response containing the header of the original packet, the parser extracts the inner payload to resolve the connection key.
3. The lookup direction is inverted (e.g. `kOutput` becomes `kInput`), allowing `LookupAndUpdate` to match the ICMP packet with the correct original session.
4. The resolved key is passed to the lambda to execute table lookup, validation, and update.
