# GreatHole Core Module (`great-hole-core`)

The `core` module provides the foundational building blocks for the GreatHole VPN application, including packet processing, obfuscation filters, network pipeline routing, and endpoint abstractions (TUN, UDP, multiplexed UDP, etc.).

## Components Overview

### 1. Endpoints (`Endpoint.hpp`)
Endpoints abstract data sources and sinks. They represent network interfaces, sockets, or protocols that can read and write packets.
- **`EndpointTun`**: Integrates with Linux/Android virtual TUN devices for intercepting IP traffic.
- **`EndpointTunSplitIp`**: Routes traffic between a virtual TUN device and multiple channels based on destination IP address routing rules.
- **`EndpointUdp`**: Standard UDP socket endpoint.
- **`EndpointUdpMux`**: A static multiplexed UDP endpoint supporting multiple virtual channels over a single UDP socket.
- **`EndpointUdpDynMux`**: A dynamic multiplexed UDP endpoint that negotiates channels and parameters dynamically.

### 2. Filters (`Filter.hpp`)
Filters process packet payloads as they flow through pipelines.
- **`FilterXor`**: Simple byte-wise XOR obfuscation filter.

### 3. Pipelines (`Pipeline.hpp`)
Pipelines connect endpoints and filters together to define the flow of packets, managing buffer allocations, routing callbacks, and fiber lifecycles.

### 4. Packet Builder & Parser (`PacketBuilder.hpp`)
A header-only declarative framework for validating, building, and parsing binary packets at compile time.
- **`PacketField`**: Concepts for numeric types, enums, raw arrays, and aliases.
- **`PacketComponentContainer`**: Containers mapping fields to layout offsets and chaining next components.
- **`PacketComponentEnumMap`**: Maps dynamic packet structures based on a leading enum indicator.

---

## Usage Guide & Examples

### Using PacketBuilder to Define and Parse Packets
To build and parse packets with compile-time safety:

```cpp
#include "PacketBuilder.hpp"

// Define custom fields
using FieldVersion = gh::PacketFieldNumeric<uint8_t>;
using FieldMsgType = gh::PacketFieldEnum<MyMsgType>;
using FieldPayload = gh::PacketFieldRaw<16>;

// Define the packet component layout
// Arguments: std::tuple<Fields...>, NextComponent
using MyPacketHeader = gh::PacketComponentContainer<
    std::tuple<FieldVersion, FieldMsgType>,
    gh::PacketComponentEnd
>;

// 1. Build a packet
std::array<uint8_t, 32> buffer{};
gh::PacketWrapper<MyPacketHeader> wrapper(buffer);

// Chain calls to set the field values sequentially
wrapper.Build()(uint8_t(1), MyMsgType::kConnect);

// 2. Parse and validate a packet
auto parserOpt = wrapper.Parse();
if (parserOpt.has_value()) {
  uint8_t version = parserOpt->Get<FieldVersion>();
  MyMsgType msgType = parserOpt->Get<FieldMsgType>();
}
```

### Setting up a Pipeline
```cpp
#include "Pipeline.hpp"
#include "EndpointTun.hpp"
#include "EndpointUdp.hpp"

// Setup endpoints
auto tun = std::make_shared<gh::EndpointTun>("tun0");
auto udp = std::make_shared<gh::EndpointUdp>("12.34.56.78", 1234);

// Connect them in a Pipeline
auto pipeline = std::make_shared<gh::Pipeline>(tun, udp);
pipeline->Start();
```
