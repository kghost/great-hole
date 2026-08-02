#pragma once

#include <windows.h>

namespace gh {

/// \brief RAII wrapper for Windows HANDLE objects that automatically calls CloseHandle on destruction.
class AutoHandle {
public:
  /// \brief Constructs an AutoHandle wrapping nullptr or a raw HANDLE.
  explicit AutoHandle(HANDLE handle = nullptr) noexcept;

  /// \brief Destructor that closes the handle if valid.
  ~AutoHandle() noexcept;

  AutoHandle(const AutoHandle&) = delete;
  auto operator=(const AutoHandle&) -> AutoHandle& = delete;

  /// \brief Move constructor.
  AutoHandle(AutoHandle&& other) noexcept;

  /// \brief Move assignment operator.
  auto operator=(AutoHandle&& other) noexcept -> AutoHandle&;

  /// \brief Checks whether the underlying handle is valid (not nullptr and not INVALID_HANDLE_VALUE).
  [[nodiscard]] auto IsValid() const noexcept -> bool;

  /// \brief Explicit boolean conversion checking validity.
  explicit operator bool() const noexcept;

  /// \brief Returns the raw handle without transferring ownership.
  [[nodiscard]] auto Get() const noexcept -> HANDLE;

  /// \brief Releases ownership of the handle without closing it and returns the raw handle.
  [[nodiscard]] auto Release() noexcept -> HANDLE;

  /// \brief Closes current handle if valid and takes ownership of newHandle.
  void Reset(HANDLE newHandle = nullptr) noexcept;

  /// \brief Closes the handle if valid and resets internal handle to nullptr.
  void Close() noexcept;

  /// \brief Resets current handle and returns a pointer to internal handle for out parameters.
  [[nodiscard]] auto Put() noexcept -> HANDLE*;

private:
  HANDLE _Handle = nullptr;
};

using UniqueHandle = AutoHandle;

} // namespace gh
