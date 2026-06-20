# ServiceBase Lifecycle and Fiber Context Guidelines

This directory contains the core service architecture and base classes for the GreatHole project. To maintain robust concurrency and lifecycle management, all components extending `ServiceBase` must adhere to these rules.

---

## 1. Implementing a Service (Subclassing `ServiceBase`)

When implementing a new service class by inheriting from `ServiceBase`, you must override the following virtual methods:

- **`DoStart()`**: Perform any initial configuration (e.g., opening sockets, binding endpoints). Return `ErrorCode{}` on success, or the respective system error code on failure.
- **`DoWork()`**: Implement the main processing loop or async event listeners.
  - *Note*: Always yield execution cooperatively. By default, the base class implementation `co_await _Service.value()._Stop.GetFiberCancelEvent();` waits indefinitely until stopped.
- **`DoGracefulStop()`**: Perform cleanup actions when stopping (e.g., closing sockets, cancelling child fibers, waiting for sub-services).

---

## 2. Using a Service

When consuming or managing the lifecycle of a `ServiceBase` instance, you must orchestrate execution using the following interface:

- **`Start()`**: Spawns the service's internal worker fiber on the current fiber and executes `DoWork()`. Returns the result of `DoStart()`.
- **`Stop()`**: Triggers the cancel event to exit the `DoWork()` loop, waits for `DoGracefulStop()` to finish, and returns its stop code.
- **`WaitService()`**: Cleans up the internal context after the service fiber has completely exited.

### Critical Fiber Context Ownership Rules
Fibers in `OmniFiber` enforce structured parent-child concurrency invariants:

1. **The Initiator owns the Service Fiber**:
   - Calling `Start()` spawns the service's worker fiber as a child of the **currently executing fiber**.
   - Therefore, calling `WaitService()` (which joins the worker fiber) **MUST** occur in the same fiber context (the owner fiber) that started the service.
   - Calling `WaitService()` from a different fiber context (e.g. starting a child service inside an RPC handler but joining it in the root test fiber) will crash with:
     `Assertion _Children.contains(child) || _FinishedChildren.contains(child) failed`.

2. **Joining Child Services Safely in Tests**:
   - In unit tests, rather than directly calling `WaitService()` on dynamically spawned/started services from the root fiber, let the parent manager clean them up under its own fiber context (e.g. calling `Stop()` on the parent multiplexer).
   - Alternatively, poll the service state (e.g., waiting for `GetState() == ServiceBase::State::kFinished`) and then call `co_await current.WaitAll()` to clean up/join children safely.

3. **Structured Cleanup**:
   - A parent fiber must not exit while it still has active or unjoined child fibers. Always call `co_await current.WaitAll()` before completing the root fiber in a test to ensure all background tasks are fully joined and cleaned up.
