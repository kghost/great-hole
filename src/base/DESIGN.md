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
