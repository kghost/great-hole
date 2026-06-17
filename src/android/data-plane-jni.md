# AndroidHole Data Plane JNI Contract & Implementation Guide

AndroidHole owns Android integration, user policy, per-app endpoint selection, and VPN lifecycle. The native data plane (`libandroidhole_dataplane.so`) owns packet I/O, tunnel protocols, encryption, endpoint sessions, packet scheduling, and forwarding.

## 1. Architecture Overview

The native library is responsible for:
- Reading and writing IP packets from the Android TUN interface (`tun_fd`).
- Encapsulating and decapsulating packets for transport to remote endpoints.
- Managing a set of tunnel endpoints.
- Routing flows to specific endpoints via Java-side callbacks.
- Maintaining background threads for non-blocking I/O.

## 2. Handle Management

Handles are passed as `jlong` in JNI. The native implementation should cast these to pointers of internal structures.

- **Allocation**: `nativeCreate` must allocate a session object and return its pointer as a `jlong`. `nativeAddEndpoint` returns a handle for an endpoint.
- **Validity**: Handles remain valid until their corresponding `nativeDestroy` (for sessions) or `nativeRemoveEndpoint` (for endpoints) call. The Java layer guarantees that `nativeDestroy` is called exactly once for each successful `nativeCreate`.
- **References**: Native code must create JNI `GlobalRef`s for the `callbacks` and `connectivity_manager` objects if it intends to use them outside the scope of `nativeCreate`. These references must be released in `nativeDestroy`.

---

## 3. Session & Endpoint Lifecycle

### `nativeCreate`
```c
JNIEXPORT jlong JNICALL
Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeCreate(
    JNIEnv* env, jclass clazz, jobject callbacks, jobject connectivity_manager);
```
- **Thread**: Called on a Java background service thread.
- **Precondition**: None.
- **Postcondition**: Returns a unique session handle. Native resources are allocated but idle. Stores a `GlobalRef` to the `callbacks` and `connectivity_manager` objects.
- **Notes**: Store the `JavaVM*` (from `env->GetJavaVM`) for later use in background threads.

### `nativeStart`
```c
JNIEXPORT void JNICALL
Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeStart(
    JNIEnv* env, jclass clazz, jlong session_handle, jint tun_fd, jint mtu, jbyteArray encryption_key);
```
- **Thread**: Called on a Java background service thread.
- **Precondition**: `session_handle` is valid and the session is not already started. `tun_fd` is an open, blocking/non-blocking file descriptor for the TUN interface.
- **Postcondition**: Background worker threads are spawned for packet processing.
- **Ownership**: Native code **takes ownership** of `tun_fd` and is responsible for closing it during `nativeStop` or `nativeDestroy`.
- **Parameters**: `encryption_key` is a dynamic length binary string used for session encryption.
- **Background Work**: This function should return quickly. All heavy I/O and crypto must happen on native-managed threads.

### Endpoint Management

#### `nativeAddEndpoint`
```c
JNIEXPORT jlong JNICALL
Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeAddEndpoint(
    JNIEnv* env, jclass clazz, jlong session_handle, jbyteArray psk, jstring host, jint port);
```
- **Returns**: A unique handle for the endpoint.
- **Parameters**: `psk` is a 16-byte pre-shared key.

#### `nativeRemoveEndpoint`
```c
JNIEXPORT void JNICALL
Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeRemoveEndpoint(
    JNIEnv* env, jclass clazz, jlong session_handle, jlong endpoint_handle);
```

#### `nativeStartEndpoint` / `nativeStopEndpoint`
```c
JNIEXPORT void JNICALL
Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeStartEndpoint(
    JNIEnv* env, jclass clazz, jlong session_handle, jlong endpoint_handle);

JNIEXPORT void JNICALL
Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeStopEndpoint(
    JNIEnv* env, jclass clazz, jlong session_handle, jlong endpoint_handle);
```

### `nativeStop`
```c
JNIEXPORT void JNICALL
Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeStop(
    JNIEnv* env, jclass clazz, jlong session_handle);
```
- **Thread**: Java background service thread.
- **Precondition**: Session is running.
- **Postcondition**: Native worker threads are signaled to stop. `tun_fd` is closed. No new callbacks will be initiated.
- **Notes**: This function should block until native threads have safely exited or transitioned to a stopped state.

### `nativeDestroy`
```c
JNIEXPORT void JNICALL
Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeDestroy(
    JNIEnv* env, jclass clazz, jlong session_handle);
```
- **Thread**: Java background service thread.
- **Precondition**: `nativeStop` has been called (or session was never started).
- **Postcondition**: All memory and global references associated with the handle are freed. The handle is no longer valid.

---

## 4. Callback Contract

Native code receives an object implementing `TunnelDataPlaneCallbacks`.

### Threading Requirements for Callbacks
1. **JNI Attachment**: Native worker threads must be attached to the JVM (via `AttachCurrentThreadAsDaemon`) before calling any JNI method.
2. **Re-entrancy**: Callbacks may be invoked from multiple native threads simultaneously. The Java implementation is responsible for its own synchronization.
3. **Blocking**: Native threads should avoid calling Java methods that block for a long time, as this will stall the data plane.

### Required Callbacks

```kotlin
fun protectSocket(socketFd: Int): Boolean
```
- **Usage**: MUST be called for every network socket created by native code before `connect()`. This ensures the tunnel traffic itself doesn't loop back into the VPN.

```kotlin
fun findTunnelForFlow(protocol: Int, localAddress: ByteArray, localPort: Int,
                      remoteAddress: ByteArray, remotePort: Int): Long
```
- **Usage**: Called when a new IP flow is seen on the TUN interface.
- **Performance**: Native side SHOULD cache the resulting endpoint handle to avoid repeated JNI overhead for every packet.
- **Parameters**: `localAddress` and `remoteAddress` are `ByteArray` (16 bytes for IPv6, IPv4 are mapped into IPv6).

```kotlin
fun onTunnelStateChanged(endpointHandle: Long, state: Int, error: String?)
```
- **Usage**: Notifies the UI/Service of life-cycle changes or errors for a specific endpoint.
- **`state` Values**:
  | Name | Value | Description |
  | :--- | :--- | :--- |
  | `Starting` | 0 | Endpoint is initializing. |
  | `Running` | 1 | Endpoint is active and processing packets. |
  | `Stopping` | 2 | Shutdown has been requested for this endpoint. |
  | `Stopped` | 3 | Endpoint has fully stopped. |
  | `Failed` | 4 | A terminal error occurred on this endpoint. |
- **`error`**: Optional error message when state is `Failed`.

```kotlin
fun onVpnStateChanged(state: Int, error: String?)
```
- **Usage**: Notifies the UI/Service of session-level life-cycle changes or errors.
- **`state` Values**: (Defined by native implementation)
- **`error`**: Optional error message.

```kotlin
fun onTrafficStats(endpointHandle: Long, txBytes: Long, rxBytes: Long)
```
- **Usage**: Periodically (e.g., every 1-5 seconds) report accumulated traffic for a specific endpoint.

---

## 5. Threading Model Summary

| Operation | Thread | Blocking? |
| :--- | :--- | :--- |
| `nativeCreate` | Java | No |
| `nativeStart` | Java | No (spawns native threads) |
| `nativeStop` | Java | Yes (waits for join) |
| `nativeDestroy` | Java | Yes |
| TUN Read/Write | Native Worker | Yes (Event loop) |
| Socket I/O / Crypto | Native Worker | Yes |
| Callbacks to Java | Native Worker | Depends on Java impl |

---

## 6. Implementation Details & Best Practices

### Per-App Endpoint Behavior
The Android VPN service can include selected packages in the VPN with `addAllowedApplication`, but it cannot assign different Android VPN interfaces per app. All selected app traffic enters the same TUN interface. Native code must classify each flow and then route the flow through the configured endpoint by calling `findTunnelForFlow`.

DNS packets should be treated as ordinary app traffic and forwarded through the endpoint selected for the flow.

### TUN Interface I/O
- **Efficiency**: On Android, the `tun_fd` is usually in blocking mode by default. It is recommended to use `poll()` or `epoll()` in native code for efficient, event-driven I/O.
- **MTU**: Respect the provided MTU to avoid packet fragmentation issues.

### JNI Performance
- **Caching**: Minimize JNI transitions by caching the `jmethodID` of callback methods in `JNI_OnLoad`.
- **Local Refs**: Be mindful of JNI local reference limits in long-running native loops. Ensure they are deleted or that `PushLocalFrame` / `PopLocalFrame` is used if many temporary objects are created.
