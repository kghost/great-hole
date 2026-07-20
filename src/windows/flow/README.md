# Windows Flow Sniffer Module

The Flow Sniffer module handles capturing network connection flow events on Windows using `WinDivert` in sniffing mode at the flow layer.

## Public APIs

### `WinDivertFlowSniffer`
Service that starts a background event loop calling `WinDivertRecvEx` at the `WINDIVERT_LAYER_FLOW` layer. It monitors connection establishment and deletion.

- **`Start()` / `Stop()`**: Standard Service controls to launch and stop the background sniffer loop.

### `WinDivertFlowSnifferCallback`
Interface to receive sniffer events:
- **`OnFlowEstablished(FlowKey key, uint32_t pid)`**: Invoked when a TCP, UDP, or ICMP flow is established. Contains the `ConnectionTracker::ConnectionKey` key and originating Process ID.
- **`OnFlowDeleted(FlowKey key)`**: Invoked when a flow is closed/deleted.

## Command Line Tools

### `flow-dump`
A command line utility to monitor and log connection flow events in the system in real-time.

#### How to Build
Run the CMake build workflow (e.g., `windows-debug-ninja` or `windows-debug-msvc`). The executable will be generated in the build output directory under `src/windows/flow/tools/flow-dump.exe`.

#### How to Run
Run the executable with Administrator privileges (required by WinDivert):
```cmd
flow-dump.exe
```
This will start monitoring network flow events and print all established and deleted flows in real-time.
