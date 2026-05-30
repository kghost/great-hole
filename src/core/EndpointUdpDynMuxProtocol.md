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

For any packet starting with `0x0000`, the 3rd byte specifies the **Message Type**. All control messages (except `INVALID_CHANNEL`) are authenticated/identified via a **16-byte Pre-Shared Key (PSK)** located starting at byte 3 (offset `3` to `18`).

### `0x01`: `INITIATE`
- **Format**: `[2-bytes: 0x0000] [1-byte: 0x01] [16-bytes: PSK] [2-byte: My Rx Channel ID] [2-byte: Peer Rx Channel ID]`
- **My Rx Channel ID**: Big-endian 16-bit integer (1-65535).
- **Peer Rx Channel ID**: Big-endian 16-bit integer (or `0` if unknown).
- **Purpose**: Initiates negotiation, requests/announces Rx IDs, and serves as session migration trigger.

### `0x03`: `KEEPALIVE`
- **Format**: `[2-bytes: 0x0000] [1-byte: 0x03] [16-bytes: PSK]`
- **Purpose**: Verifies peer is still active. Sent from both sides with a uniform random delay.

### `0x04`: `KEEPALIVE_ACK`
- **Format**: `[2-bytes: 0x0000] [1-byte: 0x04] [16-bytes: PSK]`
- **Purpose**: Sent to acknowledge a keepalive ping.

### `0x09`: `INVALID_PSK`
- **Format**: `[2-bytes: 0x0000] [1-byte: 0x09] [16-bytes: PSK]`
- **Purpose**: Sent when a control packet is received with an unrecognized/unconfigured PSK.

### `0x0A`: `INVALID_CHANNEL`
- **Format**: `[2-bytes: 0x0000] [1-byte: 0x0A] [2-byte: Received Channel ID]`
- **Purpose**: Reworked error message to prevent PSK leakage. Sent when a data packet is received with an unknown channel ID or from an unrecognized source address. Contains only the 2-byte invalid Channel ID and no PSK.

---

## 3. Connection State Machine

```
   [none] -- (async_start) --> [negotiating] <---------+
                                    |                  | (keepalive timeout OR
                                    | (assigned ID)    |  INVALID_CHANNEL matches)
                                    v                  |
                                [running] -------------+
```

- **Negotiation**: Retries sending `INITIATE` every 1 second until the peer responds with `INITIATE`.
- **Peer Address Updates**: The peer address is strictly updated only upon receiving a valid `INITIATE` packet.
- **Simultaneous Start**: If both ends initiate negotiation simultaneously, both send `INITIATE` and receive each other's Rx IDs, transitioning directly to `kRunning`.
- **Keepalive**: Both sides schedule keepalive pings with a uniform random delay between `period` (5 seconds) and `2*period` (10 seconds). Receiving a keepalive restarts the keepalive timer.
- **Error & Migration handling**: Receiving a data packet with mismatched peer IP/port triggers an `INVALID_CHANNEL` control packet to the sender. If we receive an `INVALID_CHANNEL` control packet and the returned ID matches our `_RemoteRxId` and the sender matches `_Peer`, we transition to `kNegotiating` to send `INITIATE` and update peer info.
