---
trigger: model_decision
description: "Build instructions and rules for the GreatHole project, enforcing CMake workflows"
---

# GreatHole Build Instructions & Rules

To ensure a consistent, clean, and reproducible build environment, all configurations, builds, and tests in the GreatHole repository must strictly utilize the predefined CMake workflows.

## 1. Always Use CMake Workflows

You must **never** run manual configuration or build commands directly (such as passing compiler options, build type, or custom cache variables directly to `cmake`, or running manual `cmake --build`). Instead, always use the `--workflow` flag with a predefined preset.

### Standard Build Workflow

Run the following command to configure, build, and run tests (if applicable) in a single workflow:

```bash
cmake --workflow --preset <workflow_preset_name>
```

### Exception: Running Individual Tests

When focusing on a specific problem or debugging, you are allowed to execute individual tests directly using `ctest` with the corresponding test preset and the `-R` filter flag:

```bash
ctest --preset <test_preset_name> -R <test_name_regex>
```

_(Example: `ctest --preset test-windows-ninja-debug-asan -R VpnClientMultiChannelTest`)_

---

## 2. Available Workflows

The following workflow presets are configured in `CMakePresets.json`:

### Host / Linux Workflows

- **Clang Debug (`clang-debug`)**
  - **Description:** Configures, builds, and runs unit tests using Clang in Debug mode.
  - **Command:** `cmake --workflow --preset clang-debug`

- **GCC 14 Debug (`gcc-14-debug`)**
  - **Description:** Configures, builds, and runs unit tests using GCC 14 in Debug mode.
  - **Command:** `cmake --workflow --preset gcc-14-debug`

- **GCC Snapshot Debug (`gcc-snapshot-debug`)**
  - **Description:** Configures, builds, and runs unit tests using GCC Snapshot in Debug mode.
  - **Command:** `cmake --workflow --preset gcc-snapshot-debug`

### Windows MSVC Workflows

- **Debug (`windows-msvc-debug`)**
  - **Description:** Configures and builds using MSVC in Debug mode.
  - **Command:** `cmake --workflow --preset windows-msvc-debug`

- **Debug with ASAN (`windows-msvc-debug-asan`)**
  - **Description:** Configures and builds using MSVC with AddressSanitizer in Debug mode.
  - **Command:** `cmake --workflow --preset windows-msvc-debug-asan`

- **Release (`windows-msvc-release`)**
  - **Description:** Configures and builds using MSVC in Release mode.
  - **Command:** `cmake --workflow --preset windows-msvc-release`

- **Release with ASAN (`windows-msvc-release-asan`)**
  - **Description:** Configures and builds using MSVC with AddressSanitizer in Release mode.
  - **Command:** `cmake --workflow --preset windows-msvc-release-asan`

### Windows Ninja Workflows

- **Debug (`windows-ninja-debug`)**
  - **Description:** Configures and builds using Ninja in Debug mode.
  - **Command:** `cmake --workflow --preset windows-ninja-debug`

- **Release (`windows-ninja-release`)**
  - **Description:** Configures and builds using Ninja in Release mode.
  - **Command:** `cmake --workflow --preset windows-ninja-release`

- **Debug with ASAN (`windows-ninja-debug-asan`)**
  - **Description:** Configures, builds, and runs unit tests using Ninja with AddressSanitizer in Debug mode.
  - **Command:** `cmake --workflow --preset windows-ninja-debug-asan`

- **Release with ASAN (`windows-ninja-release-asan`)**
  - **Description:** Configures, builds, and runs unit tests using Ninja with AddressSanitizer in Release mode.
  - **Command:** `cmake --workflow --preset windows-ninja-release-asan`

### Android Cross-Compilation Workflows

- **ARM64 Debug (`android-arm64-v8a-debug`)**
  - **Description:** Configures and builds the Android arm64-v8a shared library in Debug mode.
  - **Command:** `cmake --workflow --preset android-arm64-v8a-debug`

- **ARM64 Release (`android-arm64-v8a-release`)**
  - **Description:** Configures and builds the Android arm64-v8a shared library in Release mode.
  - **Command:** `cmake --workflow --preset android-arm64-v8a-release`

- **x86_64 Debug (`android-x86_64-debug`)**
  - **Description:** Configures and builds the Android x86_64 shared library in Debug mode.
  - **Command:** `cmake --workflow --preset android-x86_64-debug`

- **x86_64 Release (`android-x86_64-release`)**
  - **Description:** Configures and builds the Android x86_64 shared library in Release mode.
  - **Command:** `cmake --workflow --preset android-x86_64-release`

---

## 3. General Build Rules

- **Do not modify `CMakePresets.json`** unless explicitly requested by the user or required to add a new build configuration/workflow.
- **Clean builds:** If you need to clean or rebuild from scratch, delete the corresponding binary directory (e.g., `rm -rf build-*` for the specific preset) and re-run the workflow command.
- **Windows Build Environment:** For Windows builds, following powershell command may be needed to be run first to set the VS build environment:
  ```powershell
  Set-ExecutionPolicy -ExecutionPolicy Bypass -Scope Process
  & "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64
  ```
