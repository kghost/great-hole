# GreatHole Core Module Internal Design (`great-hole-core`)

This document describes the internal architecture, template design patterns, concurrency choices, and data flow of the `core` module.

---

## 1. PacketBuilder Design & Metaprogramming

`PacketBuilder.hpp` is a header-only library enabling zero-copy, compile-time offset computation and validation of packet headers using modern C++23 features.

### Class Layout
- **`PacketComponentContainer`**:
  - Encapsulates a layout of fields and points to the next chained component.
  - Form: `template <typename FieldsTuple, typename NextComponent> class PacketComponentContainer;`
  - Specialization: `template <typename... Fields, typename NextComponent> class PacketComponentContainer<std::tuple<Fields...>, NextComponent>`
  - Offset calculation is computed at compile-time via `consteval` lambdas using folded pack expansions and index sequences:
    ```cpp
    static constexpr const std::array<FieldInfo, sizeof...(Fields)> FieldOffsets = []() consteval {
      std::array<FieldInfo, sizeof...(Fields)> result{};
      auto process = [&]<std::size_t Is, typename Field>() {
        result[Is].Size = Field::Size;
        if constexpr (Is == 0) result[Is].Offset = 0;
        else result[Is].Offset = result[Is - 1].Offset + result[Is - 1].Size;
      };
      // ... Index sequence expansion ...
    }();
    ```

### PacketBuilder & Parser Flow
- **`PacketBuilder`**: Employs a fluent builder API where invoking `operator()` processes fields, sets the data in a zero-copy `std::span`, and returns a builder pointing to the next component:
  ```
  PacketBuilder<ComponentA> -> operator() -> PacketBuilder<ComponentB>
  ```
- **`PacketParser`**: Provides a type-safe `Get<Field>()` helper that calculates the index of `Field` inside `FieldsTuple` at compile time and reads the underlying memory with proper endian conversion.

---

## 2. Pipeline and Concurrency Model

GreatHole uses fibers for lightweight concurrency, defined in the `omni-fiber` library.

```mermaid
graph LR
    TUN[TUN Endpoint] <--> |Fiber Read/Write| Pipeline[Pipeline Routing]
    Pipeline <--> |Fiber Read/Write| UDP[UDP/Mux Endpoint]
```

- **Thread-Safety**: Endpoints execute reading and writing concurrently using dedicated reader/writer fibers. Zero-copy transfer is preferred via `std::span<uint8_t>`.
- **Lifecycles**: `Pipeline::Start` spawns read/write loops running in `omni-fiber` fibers. Stopping the pipeline cancels pending IO operations gracefully.

---

## 3. Dynamic UDP Multiplexing (`EndpointUdpDynMux`)

`EndpointUdpDynMux` implements dynamic channel multiplexing on a single UDP port.
- State machines are managed via asynchronous keepalives, version negotiation, and re-keying/address migration.
- For detailed bitwise layouts, refer to [EndpointUdpDynMuxProtocol.md](file:///home/kghost/workspace/great-hole/src/core/EndpointUdpDynMuxProtocol.md).
