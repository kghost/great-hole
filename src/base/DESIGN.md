# ServiceBase Internal Design & Concurrency Protection

This document outlines the internal design of `ServiceBase` in the `src/base` module, detailing its lifecycle management, coroutine synchronization mechanisms, and safety considerations under concurrent access.

---

## 1. Internal Architecture

Each instance of `ServiceBase` manages an asynchronous lifecycle. To keep the service's lifetime and execution state clean and resettable, its active execution context is stored within a heap-allocated `Context` structure:

```cpp
struct Context {
  Cancel _Stop;
  Omni::Fiber::Event<ErrorCode> _StopError;
  std::shared_ptr<Omni::Fiber::Fiber> _Fiber;
};
```

- **`_Stop`**: A cooperative cancellation trigger used to signal the worker fiber to stop execution.
- **`_StopError`**: An event fired when the service has stopped, containing the error code returned by `DoGracefulStop()`.
- **`_Fiber`**: A shared pointer to the worker fiber running `DoWork()`.

The lifecycle is controlled by the `Stop()` method, which performs the following operations:
1. Triggers cooperative cancellation via `_Stop`.
2. Joins the worker fiber `_Fiber` to ensure it completes.
3. Awaits the `_StopError` event to retrieve the `DoGracefulStop()` return value.
4. Resets the `_Service` context and transitions the state back to `kNone` to allow restarting.

---

## 2. Zero-Overhead `StateMachine` Design

`StateMachine` is built as a zero-overhead C++23 template class managing variant state storage and compile-time transition dispatch.

### Key Components

- **Variant Storage (`_storage`)**: State data for all defined states is stored in `std::variant<StateDataTypes...>`. The initial state's data is default-constructed using `std::in_place_index<kInitialIndex>`.
- **Fiber Synchronization (`_Mutex`)**: State transitions are serialized under `Omni::Fiber::Mutex`.
- **In-Place Construction / Destruction**: When transitioning from `StateA` to `StateB` via `Action`:
  1. The destructor for `StateA` data is automatically called by `_storage.template emplace<targetIndex>(...)`.
  2. The target state data is constructed in-place by passing the action object `TargetData(action)`.
- **Compile-Time Transition Lookup (`FindTargetState`)**:
  Matches `(FromState, ActionType)` against `TransitionsPack` at compile time to compute the target state enum value and its index in `StateDefinesPack`.
- **Coroutine Action Visitor Dispatch (`ExecuteForIndex`)**:
  `Action(visitor)` returns `Omni::Fiber::Coroutine<void>`, acquiring `_Mutex` before dispatching. It uses index-sequence fold expressions to match `_stateIndex` against state indices. It supports both synchronous visitors and coroutine visitors returning `Omni::Fiber::Coroutine<...>`, unwraps single action objects or `std::variant` action packs, and executes `ApplyAction`.
- **Type-Safe Data Access (`GetData<StateVal>()`)**:
  Returns a reference (`auto&` / `const auto&`) to the current state data. Throws `std::runtime_error` if the current state does not match `StateVal`.
- **`ActionResult<StateVal>` Deduction**:
  Filters all `Transition` declarations originating from `StateVal`, collects unique `actionType`s, and forms `std::variant<ActionTypes...>`.

