# Windows Utilities Module - Internal Design

This document details the internal design and memory management rules for `src/windows/utils`.

## 1. `AutoHandle` Design

`gh::AutoHandle` enforces single-owner RAII semantics for Win32 native kernel handles (`HANDLE`):

- **Single Ownership**: Copy constructor and copy assignment operators are deleted (`= delete`). Move semantics (`std::move`) transfer handle ownership.
- **Validity Check**: Handles are considered valid if `_Handle != nullptr && _Handle != INVALID_HANDLE_VALUE`.
- **Safe Closing**: `Close()` only executes `CloseHandle` when `IsValid()` returns true, avoiding invalid handle exception codes (`0xC0000008`) and preventing double-close errors.
- **Out Parameter Support**: `Put()` resets any existing handle and returns `HANDLE*`, allowing `AutoHandle` to receive output handles directly from Win32 APIs.

## 2. Process & String Helpers

- `WithProcessHandle`: Wraps process handle opening and scoping with `AutoHandle`.
- `GetProcessSequence` / `GetParentProcessId`: Calls `NtQueryInformationProcess` via `ntdll.dll` dynamic symbol loading.
- `ToString` / `ToWString`: Wide char string conversion functions using `MultiByteToWideChar` / `WideCharToMultiByte`.
