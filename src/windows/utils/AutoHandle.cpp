#include "AutoHandle.hpp"

#include <windows.h>

namespace gh {

AutoHandle::AutoHandle(HANDLE handle) noexcept : _Handle(handle) {}

AutoHandle::~AutoHandle() noexcept { Close(); }

AutoHandle::AutoHandle(AutoHandle&& other) noexcept : _Handle(other.Release()) {}

auto AutoHandle::operator=(AutoHandle&& other) noexcept -> AutoHandle& {
  if (this != &other) {
    Reset(other.Release());
  }
  return *this;
}

auto AutoHandle::IsValid() const noexcept -> bool { return _Handle != nullptr && _Handle != INVALID_HANDLE_VALUE; }

AutoHandle::operator bool() const noexcept { return IsValid(); }

auto AutoHandle::Get() const noexcept -> HANDLE { return _Handle; }

auto AutoHandle::Release() noexcept -> HANDLE {
  HANDLE handle = _Handle;
  _Handle = nullptr;
  return handle;
}

void AutoHandle::Reset(HANDLE newHandle) noexcept {
  if (_Handle != newHandle) {
    Close();
    _Handle = newHandle;
  }
}

void AutoHandle::Close() noexcept {
  if (IsValid()) {
    CloseHandle(_Handle);
  }
  _Handle = nullptr;
}

auto AutoHandle::Put() noexcept -> HANDLE* {
  Reset();
  return &_Handle;
}

} // namespace gh
