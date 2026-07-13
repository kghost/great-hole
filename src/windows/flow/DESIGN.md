# Windows Flow Sniffer Design Details

## Internal Architecture

The `WinDivertFlowSniffer` service opens a WinDivert handle at `WINDIVERT_LAYER_FLOW` using the following flag combinations:
- `WINDIVERT_FLAG_SNIFF`
- `WINDIVERT_FLAG_RECV_ONLY`

This allows the sniffer to run in non-blocking / sniff-only mode, meaning it receives copies of flow events without intercepting or delaying the packets.

### Concurrency and Cancellation

- **Background Loop**: Spawns a background work fiber running `DoWork()` which handles flow events asynchronously.
- **Asio Event Loop Integration**: Since Boost.Asio's `windows::object_handle` does not support cancellation slots, we register the underlying WinDivert flow handle with `Cancel` using `Cancel::HandleTracker` during `DoWork()`. When a stop is requested:
  1. `_Stop.Trigger()` executes `HandleTracker::Emit()`.
  2. This calls `CancelIoEx` on the WinDivert flow handle with the active `OVERLAPPED` structure.
  3. The pending overlapped read is canceled, which signals the overlapped event.
  4. The pending `_ReadObject->async_wait` completes, resuming the coroutine, which sees the triggered cancel token and exits `DoWork()` cleanly so the fiber can be joined.
- **Named Pipe Communication**: Under the hood, the fake `WinDivert.dll` simulates `WINDIVERT_LAYER_FLOW` by using named pipes to complete overlapped reads.

### Flow Parsing
Parses `WINDIVERT_ADDRESS` flow events for the following protocols, mapping them directly to the corresponding `ConnectionTracker::ConnectionKey` variant alternatives:
- `IPPROTO_TCP`: Mapped to `ConnectionTracker::Ip4TcpKey` or `ConnectionTracker::Ip6TcpKey`
- `IPPROTO_UDP`: Mapped to `ConnectionTracker::Ip4UdpKey` or `ConnectionTracker::Ip6UdpKey`
- `IPPROTO_ICMP`: Mapped to `ConnectionTracker::IcmpKey`
- `IPPROTO_ICMPV6`: Mapped to `ConnectionTracker::Icmp6Key`

IPv4 addresses are read as v4-mapped IPv6 addresses from the flow record, and converted back to standard IPv4 structures if mapped.
