# DesktopHole Windows Policy Engine

The Windows Policy Engine handles application-based split tunneling and routing policies on Windows. It maps connection flows back to originating process contexts using a WinDivert flow layer event loop sniffer, process snapshotting, and Event Tracing for Windows (ETW) monitoring.

---

## 1. Product Overview & Goals

DesktopHole's core packet-redirection backend (`great-hole`) handles traffic at the flow level. The Policy Engine bridges the gap between network-level flows and OS-level application contexts.

### Key Objectives
- **Granular Split Tunneling**: Allow the user to select which applications go through the VPN and which bypass it.
- **Process Tree Inheritance**: Correctly identify and apply policies to child processes spawned by a managed application (e.g., compile toolchains, debuggers, or helper commands like `git` running inside VSCode).
- **Atomic Operations**: Launch applications from the UI under a specific policy with zero packet leaks prior to policy application.
- **Low Overhead**: Resolve PID-to-flow mappings and process hierarchies with sub-millisecond latency.
- **Reused PID Safety**: Prevent policy bleeding when Windows terminates one process and assigns the same PID to a new, unrelated process.

---

## 2. Use Cases and User Scenarios

### Use Case 1: Split Tunneling by Executable Path (Persistent Policy)
* **Goal**: Automatically route or bypass all traffic of a specific application whenever it runs.
* **Actor**: End User
* **Flow**:
  1. The user opens the **App Routing** screen in the DesktopHole UI.
  2. The user adds a rule: "Route all traffic from `chrome.exe` to endpoint `US-East`."
  3. The user starts Chrome from their desktop or start menu.
  4. The policy engine detects `chrome.exe` starting, monitors its flows, and routes them to `US-East`.
  5. The user adds another rule: "Bypass VPN for `spotify.exe`." Spotify traffic goes directly through the local internet.

### Use Case 2: Ad-Hoc Policy on Running Process (Dynamic PID Routing)
* **Goal**: Temporarily change the routing of an already running process without modifying persistent configurations.
* **Actor**: End User / Developer / Power User
* **Flow**:
  1. The user has a long-running process (e.g., a download manager or database migration script with PID `8412`).
  2. The user opens the **Active Processes** dashboard in the UI.
  3. The user locates PID `8412` and changes its policy to **ByPass**.
  4. The policy engine instantly redirects all new and active connections from PID `8412` to bypass the VPN.
  5. When PID `8412` exits, the policy is discarded.

### Use Case 3: Process Subtree Policy (Tree Inheritance)
* **Goal**: Apply a routing policy to a parent application and guarantee it propagates to all descendant processes.
* **Actor**: End User / Developer / Sysadmin
* **Flow**:
  1. The user configures a policy for `code.exe` (VSCode) with the **Subtree** scope.
  2. The user starts VSCode.
  3. Inside VSCode, the user runs a build script, which spawns `npm.exe`, `node.exe`, and `tsc.exe`.
  4. The user also runs a Git sync, which spawns `git.exe` and `ssh.exe`.
  5. The policy engine detects that these processes are descendants of VSCode (via parent-child mapping) and automatically applies the VSCode policy to all of them.

### Use Case 4: Atomic Start from UI (Leak-Free Execution)
* **Goal**: Launch an application directly from the VPN UI under a specific policy and guarantee that not a single packet leaks to the default network before the policy is applied.
* **Actor**: Security-conscious User / QA Tester
* **Flow**:
  1. The user goes to the **App Launcher** panel in the UI.
  2. The user inputs the path to a CLI tool `curl.exe -X POST https://api.internal.corp/` and selects the `Corp-Intranet` VPN endpoint.
  3. The user clicks **Run**.
  4. The engine spawns the process in a suspended state, registers its PID and subtree policy, and then resumes execution.
  5. The process executes, and its very first DNS lookup and TCP handshake are successfully routed through `Corp-Intranet`.

---

## 3. Functional Requirements

### 3.1. Policy Specification
A policy rule is defined by the following schema:

| Attribute | Type | Description |
| :--- | :--- | :--- |
| `Id` | UUID / String | Unique identifier for the policy rule. |
| `TargetType` | Enum | `ExecutablePath` (persistent matches), `ProcessId` (ad-hoc dynamic matches), or `JobInstance` (UI-launched). |
| `TargetValue` | String / Int | Path to the executable (e.g., `C:\bin\app.exe`) or the numeric `PID`. |
| `Scope` | Enum | `SingleProcess` (applies to target PID only) or `ProcessSubtree` (applies to target and all descendants). |
| `Action` | Enum | `Endpoint` (route to tunnel), `ByPass` (bypass VPN), or `Discard` (silently drop packets). |
| `EndpointId` | Int64 | The handle of the target VPN endpoint (only applicable if `Action` is `Endpoint`). |

### 3.2. Flow-to-PID Mapping
- The engine must intercept new connection attempts (TCP handshakes, UDP socket binds, DNS requests) and resolve the originating Windows `ProcessId`.
- The mapping must be real-time and cacheable.

### 3.3. Process Tree Tracking
- The engine must maintain an active in-memory representation of the OS process tree.
- When a process starts, the engine must capture:
  - `ProcessId`
  - `ParentProcessId`
  - `ExecutablePath`
- The engine must dynamically evaluate policy inheritance:
  - If a process `P` starts, its policy is checked.
  - If no direct policy matches, its parent `P_parent` is checked. If `P_parent` has a `ProcessSubtree` policy, `P` inherits it.
  - This traversal continues recursively up to the root of the process tree.

### 3.4. Cleanup and Recycled PIDs
- The engine must listen to process exit events.
- When a process terminates, its PID-to-policy mapping and node in the process tree must be immediately deleted.
- > [!IMPORTANT]
  > Because Windows recycles PIDs rapidly, failure to purge terminated processes immediately will cause "policy hijacking," where a new, unrelated process receives the routing rules of the exited process.

---

## 4. Non-Functional & Performance Requirements

- **Latency Overhead**: Resolving the routing mark for a new flow must complete in **less than 1ms**.
- **CPU & Memory Overhead**:
  - The in-memory process tree must consume negligible memory (typically < 10 MB for a normal developer workstation with ~500 active processes).
  - Monitoring process lifecycles must not exceed 0.5% CPU utilization.
- **Privilege Boundaries**:
  - The policy engine service runs inside the elevated Tauri host process (since WinDivert and process-tracking APIs require Administrator privileges).
  - The UI must communicate rules and launch commands over Tauri's IPC bridge.
- **Reliability & Fail-Safe**:
  - If the policy engine service crashes, the data plane must fall back to a safe default action (e.g., global Bypass or global Tunnel, depending on user settings) rather than freezing all network operations.

---

## 5. Integration API

The `PolicyEngine` is implemented as a `ServiceBase` subclass and exposes the following public member functions for policy registration and lifecycle management:

- `Start()` / `Stop()`: Standard `ServiceBase` service controls. Starting `PolicyEngine` automatically initializes and launches both the `ProcessTreeTracker` (ETW session) and `WinDivertFlowSniffer` sub-services.
- `ClearRegistry()`: Clears all registered path routing rules and resets the default route.
- `AddPathBypassRule(const std::string& path, PolicyScope scope)`: Registers a bypass rule for a specific executable path.
- `AddPathEndpointRule(const std::string& path, const std::shared_ptr<Session>& endpoint, PolicyScope scope)`: Registers a custom VPN tunnel endpoint route rule for a specific executable path.
- `RemovePathRule(const std::string& path)`: Removes the registered rule for the given executable path.
- `AddPidEndpointRule(DWORD pid, const std::shared_ptr<Session>& endpoint, PolicyScope scope)`: Directly registers a policy rule for a running process.
- `SetDefaultEndpoint(const std::shared_ptr<Session>& endpoint)`: Configures the default fallback routing endpoint.
- `SetDefaultBypass()`: Configures the default fallback routing to bypass the VPN.
- `LaunchWithPolicy(const std::string& commandLine, const std::shared_ptr<Session>& endpoint, PolicyScope scope)`: Spawns a process suspended, registers its process ID and rule with the trackers, and resumes the thread to guarantee atomic, leak-free routing.

---

## 6. Policy Non-Collision Constraint

To guarantee deterministic behavior and optimize tree resolution speed:
* **No Colliding Policies**: You must never register multiple conflicting policies within the same process tree hierarchy. Registering competing policies on ancestor and descendant processes is undefined behavior.
* **Subtree Dominance**: A policy configured with `Scope = PolicyScope::ProcessSubtree` dominates the entire process tree. Descendant processes always inherit the parent's subtree policy and cannot be overridden by conflicting sub-policies. Overriding a subtree policy is only supported via a bypass policy with subtree scope.
