# Symmetric UDP Dynamic Multiplexing Protocol

This document defines the symmetric protocol for dynamic receiving channel ID negotiation, keepalive session management, address migration, and error pings under the `great-hole` UDP dynamic multiplexing scheme.

---

## 1. General Framing & Multiplexing

All multiplexed packets sent over the UDP socket start with a **2-byte Channel ID** in big-endian (network byte order):
- **Channel ID `0x0000`**: Reserved for the Control & Negotiation Channel.
- **Channel ID `0x0001` to `0xFFFF`**: Data Channels. The rest of the packet contains pure, raw payload data.

Because data channels have a 2-byte prefix and do not have any internal headers or parsing overhead, they maintain 100% zero-overhead data transfer.

Under this protocol, the receiving Channel ID can be different on each end:
- End A receives data on its **Local Rx Channel ID** ($ID_A$), so End B must prefix packets to A with $ID_A$.
- End B receives data on its **Local Rx Channel ID** ($ID_B$), so End A must prefix packets to B with $ID_B$.

---

## 2. Control Messages (Channel ID `0x0000`)

For any packet starting with `0x0000`, the 3rd byte specifies the **Message Type**. All control messages (except `INVALID_CHANNEL` and `INVALID_ADDRESS`) are authenticated/identified via a **16-byte Pre-Shared Key (PSK)** located starting at byte 3 (offset `3` to `18`).

### `0x01`: `INITIATE`
- **Format**: `[2-bytes: 0x0000] [1-byte: 0x01] [16-bytes: PSK] [2-byte: My Rx Channel ID] [2-byte: Peer Rx Channel ID] [1-byte: Major] [1-byte: Minor] [2-byte: Patch]`
- **My Rx Channel ID**: Big-endian 16-bit integer (1-65535).
- **Peer Rx Channel ID**: Big-endian 16-bit integer (or `0` if unknown).
- **Major / Minor / Patch**: Protocol version info.
- **Purpose**: Initiates negotiation, requests/announces Rx IDs, serves as session migration trigger, and negotiates version compatibility.

### `0x02`: `INITIATE_FAIL`
- **Format**: `[2-bytes: 0x0000] [1-byte: 0x02] [16-bytes: PSK]`
- **Purpose**: Sent when negotiation fails due to incompatible major/minor versions.

### `0x03`: `KEEPALIVE`
- **Format**: `[2-bytes: 0x0000] [1-byte: 0x03] [16-bytes: PSK] [1-byte: Flags]`
- **Flags**: 8-bit bitmask:
  - Bit 0 (`0x01`): `Ping` flag
  - Bit 1 (`0x02`): `Pong` flag
- **Purpose**: Verifies peer activity and measures round-trip time (RTT). Utilizes a 3-packet exchange:
  1. Initiator sends keepalive with `Ping=1, Pong=0`.
  2. Receiver responds with `Ping=1, Pong=1` (acting as a Pong and starting its own Ping).
  3. Initiator responds with `Ping=0, Pong=1` (completing RTT measurement on both ends).

### `0x09`: `INVALID_PSK`
- **Format**: `[2-bytes: 0x0000] [1-byte: 0x09] [16-bytes: PSK]`
- **Purpose**: Sent when a control packet is received with an unrecognized/unconfigured PSK.

### `0x0A`: `INVALID_CHANNEL`
- **Format**: `[2-bytes: 0x0000] [1-byte: 0x0A] [2-byte: Received Channel ID]`
- **Purpose**: Sent when a data packet is received with an unknown/unconfigured channel ID. Contains only the 2-byte invalid Channel ID and no PSK.

### `0x0B`: `INVALID_ADDRESS`
- **Format**: `[2-bytes: 0x0000] [1-byte: 0x0B] [2-byte: Received Channel ID]`
- **Purpose**: Sent when a data packet is received with a known channel ID but from an unrecognized source address.

---

## 3. Connection State Machine

```
   [none] -- (async_start) --> [negotiating] <---------+
                                    |                  | (keepalive timeout OR
                                    | (assigned ID)    |  INVALID_CHANNEL matches)
                                    v                  |
                                [running] -------------+
```

- **Negotiation**: Retries sending `INITIATE` every 1 second until the peer responds with compatible `INITIATE`.
- **Version Compatibility**: On receiving `INITIATE`, we check if the peer's major and minor version matches ours. If not compatible, we respond with `INITIATE_FAIL` and stop/fail the channel.
- **Peer Address Updates**: The peer address is strictly updated only upon receiving a valid `INITIATE` packet.
- **Simultaneous Start**: If both ends initiate negotiation simultaneously, both send `INITIATE` and receive each other's Rx IDs, transitioning directly to `kRunning`.
- **Keepalive**: Both sides periodically send `KEEPALIVE` with a random delay. Receiving a Keepalive with Pong flag allows calculation of RTT.
- **Error & Migration handling**:
  - Receiving a data packet with mismatched peer IP/port triggers `INVALID_ADDRESS` (if channel ID is known) or `INVALID_CHANNEL` (if channel ID is unknown).
  - If we receive `INVALID_CHANNEL` and the returned ID matches our `_RemoteRxId` and the sender matches `_Peer`, we reset `_Peer = std::nullopt` and transition to `kNegotiating` to re-resolve and renegotiate.
  - If we receive `INVALID_ADDRESS` and the ID matches our `_RemoteRxId` and the sender matches `_Peer`, we transition to `kNegotiating` to renegotiate directly without clearing `_Peer` (not resolving).
