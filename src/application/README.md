# GreatHole Application Module

The `great-hole-application` module provides high-level application components for managing VPN servers, multi-channel VPN clients, and connection tracking.

## Components

### 1. ConnectionTracker
`ConnectionTracker` tracks active network connections (TCP, UDP, ICMP) to map them to specific `ConnectionMark` instances (e.g., VPN sessions). This enables routing reply traffic and associating related control/error messages (like ICMP Destination Unreachable) back to their originating channel.

#### Public API

*   **`LookupAndUpdate`**:
    Looks up an existing tracking entry or registers/selects a new one.
    ```cpp
    template <typename Direction>
    std::expected<std::shared_ptr<ConnectionMark>, ErrorCode>
    LookupAndUpdate(const Packet& packet, Selector& selector);
    ```
    *   `Direction`: Specifies if the packet is `ConnectionDirectionOutput` (outgoing) or `ConnectionDirectionInput` (incoming).
    *   `selector`: A reference to a `Selector` interface used to select a new mark when no tracked entry exists or the existing entry is invalid.
    *   Returns the matched or registered `ConnectionMark` shared pointer, or an `ErrorCode` if parsing or tracking failed.

*   **`Clear`**:
    Clears all tracking tables.
    ```cpp
    void Clear();
    ```

### 2. VpnClientMultiChannel
`VpnClientMultiChannel` manages multi-channel client connections, dynamically multiplexing/demultiplexing traffic across multiple UDP tunnels.

*   Integrates with `ConnectionTracker` to trace incoming/outgoing packets.
*   Supports runtime TUN migration and dynamic session life cycles.

---

## Integration Guidelines

To use `ConnectionTracker` in your pipelines:
1. Create a `Selector` subclass that returns a default/desired `ConnectionMark` when an unknown connection is initiated.
2. Initialize `ConnectionTracker` with an executor and a reference to your `Selector`.
3. Call `Start()` to run background connection pruning tasks.
4. Pass packets to `LookupAndUpdate` inside read/write pipelines to associate them with the appropriate channels.
