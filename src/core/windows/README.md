# Windows Network Redirection (WinDivert)

The Windows network redirection implementation provides the Windows-specific network data plane redirection using the WinDivert driver. It exposes a `EndpointWinDivert` endpoint, which acts as a TUN device implementation using user-mode packet redirection.

## Integration & CMake Usage

This module is integrated directly into the `great-hole-core` static library targets on Windows platforms:
- `great-hole-core`: The standard release/debug static library.
- `great-hole-core-asan`: The address-sanitized static library.

To link against this implementation, simply link against `great-hole-core` in your CMake configuration:

```cmake
# Standard variant
target_link_libraries(your-target PRIVATE great-hole-core)

# ASAN variant
target_link_libraries(your-target-asan PRIVATE great-hole-core-asan)
```

## Public API Reference

The primary class exposed by this module is [EndpointWinDivert](file:///q:/Projects/great-hole/src/core/windows/EndpointWinDivert.hpp).

### `gh::EndpointWinDivert`

A `gh::Endpoint` implementation that handles network traffic capturing and injection on Windows.

- **Constructor**:
  ```cpp
  explicit EndpointWinDivert(boost::asio::any_io_executor executor, std::string const& name);
  ```
  - `executor`: The Boost.Asio I/O executor to associate with asynchronous operations.
  - `name`: A descriptive name identifier for logging and debugging purposes.

- **Methods**:
  - `void SetConnectionTracker(std::shared_ptr<ConnectionTracker> tracker) override`:
    Registers the connection tracker instance used to evaluate routes (`Bypass`, `Discard`, or redirection mark).
  - `Omni::Fiber::Coroutine<ErrorCode> Read(Packet& p, Cancel& c) override`:
    Asynchronously reads a captured network packet from the WinDivert handle.
  - `Omni::Fiber::Coroutine<ErrorCode> Write(Packet& p, Cancel& c) override`:
    Asynchronously writes/injects a network packet back into the Windows network stack.

## Dependencies

- **WinDivert**: The WinDivert user-mode packet redirection library.
- **Boost**: Uses Boost.Asio for asynchronous I/O and Boost.Log for diagnostics.
- **omni-fiber**: Integration with the dynamic fiber execution runtime.
