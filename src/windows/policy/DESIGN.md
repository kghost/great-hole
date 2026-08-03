# DesktopHole Windows Policy Engine Internal Design

This document details the internal architecture, design decisions, data structures, and implementation details of the **DesktopHole Policy Engine**. The policy engine maps network flows to routing decisions (Bypass, Endpoint) by resolving flow-to-process associations and evaluating process hierarchy rules on Windows.

---

## 1. System Architecture

The policy engine operates within the elevated Tauri core host process. It integrates with `great-hole`'s packet capture pipeline by implementing the `gh::ConnectionTracker::Selector` interface.

```mermaid
graph TD
    subgraph UI WebView (React App)
        AppRulesUI[App Rules UI]
        AppLauncherUI[App Launcher UI]
    end

    subgraph Tauri Core Process (Elevated Host)
        TauriCmd[Tauri Commands - Rust]
        FFI[FFI Bridge - Rust/C++]
        
        subgraph Policy Engine (C++)
            Registry[PolicyRegistry]
            TreeTracker[ProcessTreeTracker]
            FlowTracker[FlowTracker]
            Selector[PolicySelector / Engine]
        end

        subgraph C++ Background Thread (Asio Loop)
            ConnTrack[gh::ConnectionTracker]
            DataPlane[gh::TunnelDataPlane]
        end
    end

    subgraph OS Kernel / APIs
        ETW[ETW Process Trace Session]
        Toolhelp[Toolhelp Snapshot API]
        DivertFlow[WinDivert flow layer]
        DivertNet[WinDivert network layer]
    end

    %% Configuration & Launch
    AppRulesUI -->|Update Rules| TauriCmd
    AppLauncherUI -->|Launch App| TauriCmd
    TauriCmd -->|FFI Call| FFI
    FFI -->|Configures| Registry
    FFI -->|Atomic CreateProcess| TreeTracker

    %% OS Inputs to Trackers
    ETW -->|Process Start/Stop Events| TreeTracker
    Toolhelp -->|Initial Process Snapshot| TreeTracker
    DivertFlow -->|Flow PID mapping| FlowTracker

    %% Policy association when PID is discovered
    TreeTracker -->|Query Rules on Start| Registry
    Registry -->|Associate policy to PID node| TreeTracker

    %% Packet Processing
    DivertNet <-->|Intercept Packets| Selector
    Selector <-->|Lookup/Update Flow| ConnTrack
    Selector -->|1. Lookup PID| FlowTracker
    Selector -->|2. Get Policy| TreeTracker
```

---

## 2. Event Loop & Concurrency Model

- All core components (`ConnectionTracker`, `FlowTracker`, `ProcessTreeTracker`, `PolicyRegistry`) execute sequentially on a single thread (the Boost.Asio event loop executor).
- Asynchronous callbacks (e.g. ETW event thread, WinDivert flow thread) delegate event processing back to the executor using `boost::asio::post` or the `EventQueue` task dispatcher.
- This architecture enables a completely lock-free (`std::shared_mutex` removed from all trackers and registries), single-threaded execution model for core data structure access, eliminating data races and deadlocks.

---

## 3. Component Design

### 3.1. Flow Tracker (`gh::policy::FlowTracker`)
The `FlowTracker` maps active network connections (represented by connection keys) to their originating Process ID (PID).
- **Driver Layer & Table Lookups**:
  - Receives socket events via `WinDivertFlowSniffer` (`WINDIVERT_LAYER_SOCKET`).
  - When `GetPidForConnection` is queried for a connection not present in `_FlowToPid`, `FlowTracker` queries the Windows IP helper tables (`GetExtendedTcpTable` / `GetExtendedUdpTable` for IPv4/IPv6 TCP and UDP) to discover the owning PID on demand and caches it.
- **Flow Cache**:
  - Maintains a map: `FlowKey -> ProcessId`.
  - On `OnFlowEstablished`, it adds/updates the mapping.
  - On `OnFlowDeleted`, it removes the mapping.
  - On `GetPidForConnection`, if missing from cache, it queries system TCP/UDP tables and caches the PID if found.

### 3.2. Process Tree Tracker (`gh::policy::ProcessTreeTracker`)
The `ProcessTreeTracker` maintains the active Windows process tree and evaluates hierarchical policy rules.
- **Race-Free Initialization Order**:
  1. Queries all running ETW sessions using `QueryAllTracesW` and calls `ControlTraceW` to stop any orphaned sessions starting with `DesktopHoleProcessTrace_` (e.g. from previous crashed/unterminated runs). This prevents ETW session slot exhaustion (error `1450` / `ERROR_NO_SYSTEM_RESOURCES`).
  2. Starts the ETW real-time session (`StartTraceW` and `EnableTraceEx2`) on the main startup thread. This ensures that the kernel starts buffering process creation/destruction events in the session's queue immediately.
  3. Captures a snapshot of currently running processes using `EnumProcesses` to build the initial `ProcessSequence -> ProcessNode` map via `GetProcessSequence` (`NtQueryInformationProcess` info class 92).
  4. Spawns a background thread running the ETW consumer session to call `OpenTraceW` and `ProcessTrace` and consume the buffered events.
  - This order eliminates the gap where process events starting in-between snapshot creation and ETW activation would be lost. Since the ETW session buffers events starting from step 1, any processes that started during the snapshot are processed in chronological order. Duplicate start events are safely ignored using `try_emplace`.
- **ETW Monitoring & On-Demand Fallback**:
  - Subscribes to the `Microsoft-Windows-Kernel-Process` provider (GUID `{22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}`).
  - **Process Start (Event ID 1)**: Adds a new node containing the process path, `ProcessSequence`, parent `ProcessSequence`, and `ProcessId`.
  - **Process Stop (Event ID 2)**: Purges the node by sequence.
  - **On-Demand Query**: If `GetAction(pid)` is called for a process not yet in `_ProcessIdMap`, `ProcessTreeTracker` inspects the process (and parent ancestor chain if missing) via `WithProcessHandle` (`OpenProcess`), builds the parent ancestor chain, adds them to `_ProcessMap` and `_ProcessIdMap`, and evaluates policies on demand.
- **PID Recycling Protection**:
  - > [!IMPORTANT]
    > PIDs are recycled rapidly by Windows. Nodes in `_ProcessMap` are keyed by 64-bit monotonically increasing `ProcessSequence` numbers. A secondary map `_ProcessIdMap` (`std::map<Interface::ProcessId, std::reference_wrapper<ProcessNode>>`) maintains the current active mapping from PID to `ProcessNode`. `AddProcess` updates `_ProcessIdMap` via `insert_or_assign`, and `RemoveProcess` conditionally erases from `_ProcessIdMap` only if the mapping matches the sequence being removed.

### 3.3. Policy Registry (`gh::policy::PolicyRegistry`)
The `PolicyRegistry` stores user-defined rules.
- **Path Rules**: Maps normalized executable paths or wildcards (e.g. `C:\Program Files\Git\bin\git.exe` or `*/git.exe`) to a routing action (`ByPassRoute`, `EndpointRoute`).
- **Default Route**: Defines the default route when no path-specific rules match.

### 3.4. Policy Selector (`gh::policy::PolicySelector`)
The `PolicySelector` implements the `ConnectionTracker::Selector` and `WinDivertRouteCallback` interfaces, and coordinates policy resolution:
1. It implements `WinDivertRouteCallback::WinDivertRoute`. When WinDivert captures outbound packets, it routes them through the `ConnectionTracker`.
2. It queries `FlowTracker` for the `ProcessId` of the connection key.
3. If the PID is found, it queries `ProcessTreeTracker` for the policy associated with that PID.
4. If either the flow mapping or the process policy isn't available yet, the selector returns a `Deferred` connection mark and registers callbacks to resume the connection once the information is resolved.

---

## 4. Out-Of-Order Event Resolution (The 6 Permutations)

Three distinct events determine connection routing:
- **P**: Packet arrival from WinDivert Network layer
- **F**: Flow arrival from WinDivert Flow layer (provides mapping: connection key -> PID)
- **Pr**: Process Start/Stop from ETW (provides mapping: PID -> executable path & policy)

Since WinDivert and ETW events are asynchronous, they can arrive in any order, leading to 6 possible scenarios:

1. **Best Case (F -> Pr -> P or Pr -> F -> P)**: Flow and Process info are already resolved when the packet arrives. The packet is routed immediately without delay.
2. **Case F -> P -> Pr**: Flow establishes PID, but Process ETW start has not arrived. 
   - *Behavior*: Packet is marked `Deferred` and queued. A resumer lambda is registered in `ProcessTreeTracker` for the PID. When the Process start event arrives, `ProcessTreeTracker` resolves the rule, transforms it into a `ConnectionMark`, and triggers the resumer to update the connection state and inject/send the queued packet.
3. **Case Pr -> P -> F**: Process exists, but Flow establishing the connection PID has not arrived.
   - *Behavior*: Packet is marked `Deferred` and queued. A resumer is registered in `FlowTracker` for the connection key. When Flow arrives, `FlowTracker` resolves the policy immediately (since Process is already cached) and triggers the resumer.
4. **Case P -> F -> Pr**: Packet arrives before Flow and Process.
   - *Behavior*: Packet is marked `Deferred` and queued; resumer registered in `FlowTracker`. When Flow arrives, `FlowTracker` registers the resumer in `ProcessTreeTracker` for the PID since the process hasn't arrived. When Process eventually arrives, it resolves policy and triggers the resumer.
5. **Case P -> Pr -> F**: Packet arrives, then Process, then Flow.
   - *Behavior*: Packet is marked `Deferred` and queued; resumer registered in `FlowTracker`. Process arrives and is cached. When Flow arrives, `FlowTracker` resolves the policy directly and triggers the resumer.

### Pending Queue & Resumer Implementation

- **`VpnClientMultiChannel::Mark::Deferred`**: Stores queued packets and their `WINDIVERT_ADDRESS` in a `std::shared_ptr<Context>`.
- **`ProcessTreeTrackerContinue` / `FlowTrackerContinue`**: Handles updating the deferred mark in-place (avoiding expensive hash map table lookups) and injects the queued packets back into the dataplane.

---

## 5. C++ Class Specifications

Below are the header sketches of the core policy components.

```cpp
namespace gh::policy {

enum class PolicyScope : std::uint8_t {
    SingleProcess,
    ProcessSubtree
};

struct PolicyRule {
    struct ByPassRoute {};
    struct EndpointRoute {
        std::weak_ptr<VpnClientMultiChannelSession> Endpoint;
    };

    using RoutingAction = std::variant<ByPassRoute, EndpointRoute>;

    RoutingAction Action;
    PolicyScope Scope = PolicyScope::SingleProcess;
};

struct ProcessNode {
    Interface::ProcessSequence ProcessSequence{0};
    std::optional<Interface::ProcessSequence> ParentProcessSequence{0};
    Interface::ProcessId ProcessId{0};
    std::optional<std::string> ExecutablePath;
    std::optional<PolicyRule> Policy;
    std::set<Interface::ProcessSequence> Children;
};

class ProcessTreeTracker : public ServiceBase {
public:
    explicit ProcessTreeTracker(boost::asio::any_io_executor executor, const PolicyRegistry& registry);
    ~ProcessTreeTracker() override = default;

    auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
    auto DoWork() -> Omni::Fiber::Coroutine<void> override;
    auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

    auto RegisterProcessPolicy(Interface::ProcessSequence process, const PolicyRule& rule)
        -> std::expected<void, std::string>;
    auto LaunchWithPolicy(const std::string& imagePath, const std::optional<std::string>& commandLine,
                          const PolicyRule& policy) -> std::expected<Interface::ProcessSequence, std::string>;
    void ApplyPathRule(const std::string& imagePath, const PolicyRule& rule);
    auto AddProcess(Interface::ProcessSequence process, Interface::ProcessSequence parentSeq, Interface::ProcessId pid,
                    std::optional<std::string> path) -> const ProcessNode&;
    void RemoveProcess(Interface::ProcessSequence process);
    auto GetAction(Interface::ProcessId process) -> std::optional<PolicyRule::RoutingAction>;
    auto GetProcessTree() const -> std::vector<Interface::ProcessInfo>;

private:
    void BuildInitialSnapshot();
    void EtwThreadProc();
    void HandleEtwEvent(PEVENT_RECORD record);

    boost::asio::any_io_executor _Executor;
    std::map<Interface::ProcessSequence, ProcessNode> _ProcessMap;
    std::map<Interface::ProcessId, std::reference_wrapper<ProcessNode>> _ProcessIdMap;
    TRACEHANDLE _EtwSessionHandle = 0;
    std::thread _EtwThread;
};

struct FlowRecord {
  ConnectionTracker::ConnectionKey Key;
  DWORD ProcessId;
};

class FlowTracker : public WinDivertFlowSnifferCallback {
public:
  explicit FlowTracker() = default;
  ~FlowTracker() override = default;

  auto OnFlowEstablished(FlowKey key, uint32_t pid) -> Omni::Fiber::Coroutine<void> override;
  auto OnFlowDeleted(FlowKey key) -> Omni::Fiber::Coroutine<void> override;

  auto GetPidForConnection(const ConnectionTracker::ConnectionKey& key) -> std::optional<DWORD>;
  auto GetFlows() const -> std::vector<FlowRecord>;

private:
  std::map<FlowKey, DWORD> _FlowToPid;
};

} // namespace gh::policy
```

---

## 6. Atomic Launch Design (UI Spawning)

To ensure leak-free atomic startup of applications under a specific policy:
1. The Rust Tauri layer calls FFI `desktophole_launch_with_policy`.
2. The C++ engine executes `CreateProcessW` with the `CREATE_SUSPENDED` flag.
3. This creates the process container in the kernel and returns its `dwProcessId`, but does not execute any instructions.
4. The C++ engine immediately registers the `dwProcessId` in the `ProcessTreeTracker` with the requested `PolicyRule` and scope (calling `AddProcess` and `RegisterPidPolicy`).
5. The engine calls `ResumeThread` on the process's primary thread.
6. The process runs. The very first packet/flow it issues will be captured by WinDivert, resolved to the new PID, and successfully routed via the pre-registered policy.
