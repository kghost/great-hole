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

_(Example: `ctest --preset debug -R Pipeline`)_

---

## 2. Available Workflows

The following workflow presets are configured in `CMakePresets.json`:

### Host Debug (`debug`)

- **Description:** Configures (using `debug` configure preset), builds (using `build-debug` build preset), and tests (using `debug` test preset) on the host environment using Clang.
- **Workflow Command:**
  ```bash
  cmake --workflow --preset debug
  ```

### Host Debug (gcc-snapshot) (`debug-gcc-snapshot`)

- **Description:** Configures (using `debug-gcc-snapshot` configure preset), builds (using `build-debug-gcc-snapshot` build preset), and tests (using `debug-gcc-snapshot` test preset) using GCC Snapshot.
- **Workflow Command:**
  ```bash
  cmake --workflow --preset debug-gcc-snapshot
  ```

### Android arm64-v8a (`android-arm64-v8a`)

- **Description:** Configures (using `android-arm64-v8a` configure preset) and builds (using `build-android-arm64-v8a` build preset) the Android arm64-v8a JNI shared library.
- **Workflow Command:**
  ```bash
  cmake --workflow --preset android-arm64-v8a
  ```

### Android x86_64 (`android-x86_64`)

- **Description:** Configures (using `android-x86_64` configure preset) and builds (using `build-android-x86_64` build preset) the Android x86_64 JNI shared library.
- **Workflow Command:**
  ```bash
  cmake --workflow --preset android-x86_64
  ```

### Host Debug (msvc) (`debug-msvc`)

- **Description:** Configures (using `debug-msvc` configure preset), builds (using `build-debug-msvc` build preset), and tests (using `debug-msvc` test preset) on Windows using Visual Studio.
- **Workflow Command:**
  ```bash
  cmake --workflow --preset debug-msvc
  ```

### Host Debug (msvc-ninja) (`debug-msvc-ninja`)

- **Description:** Configures (using `debug-msvc-ninja` configure preset), builds (using `build-debug-msvc-ninja` build preset), and tests (using `debug-msvc-ninja` test preset) on Windows using Ninja.
- **Workflow Command:**
  ```bash
  cmake --workflow --preset debug-msvc-ninja
  ```

---

## 3. General Build Rules

- **Do not modify `CMakePresets.json`** unless explicitly requested by the user or required to add a new build configuration/workflow.
- **Clean builds:** If you need to clean or rebuild from scratch, delete the corresponding binary directory (e.g., `rm -rf build` for the `debug` preset) and re-run the workflow command.
- **Build Outputs:** Always ensure your build output directory is ignored by Git (already covered in `.gitignore` for `build` and `build-*/`).
- **Windows Build Environment:** For Windows builds, following powershell command may be needed to be run first to set the VS build environment:
  ```powershell
  Set-ExecutionPolicy -ExecutionPolicy Bypass -Scope Process
  & "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64
  ```
