---
trigger: model_decision
description: "Build instructions and rules for the GreatHole project, enforcing CMake presets"
---

# GreatHole Build Instructions & Rules

To ensure a consistent, clean, and reproducible build environment, all builds and configurations in the GreatHole repository must strictly utilize the predefined CMake presets.

## 1. Always Use CMake Presets

You must **never** run manual configuration commands (e.g., passing compiler options, build type, or custom cache variables directly to `cmake .` or `cmake -B`). Instead, always use the `--preset` flag to configure the project.

### Standard Build Workflow

1. **Configure the build:**
   ```bash
   cmake --preset <preset_name>
   ```
2. **Build the target:**
   ```bash
   cmake --build --preset <build_preset_name>
   ```
   *(where `<build_preset_name>` is the build preset corresponding to the configure preset)*

---

## 2. Available Presets

The following presets are configured in `CMakePresets.json`:

### Host Debug (`debug`)
- **Description:** Default debug build configuration for the host environment.
- **Compiler:** Clang (`clang` / `clang++`).
- **Binary Directory:** `build`
- **Configuration Command:**
  ```bash
  cmake --preset debug
  ```
- **Build Preset:** `build-debug`
- **Build Command:**
  ```bash
  cmake --build --preset build-debug
  ```

### Host Debug (gcc-snapshot) (`debug-gcc-snapshot`)
- **Description:** Debug build using GCC Snapshot.
- **Compiler:** GCC Snapshot (`gcc-snapshot` / `g++-snapshot`).
- **Binary Directory:** `build-gcc-snapshot`
- **Configuration Command:**
  ```bash
  cmake --preset debug-gcc-snapshot
  ```
- **Build Preset:** `build-debug-gcc-snapshot`
- **Build Command:**
  ```bash
  cmake --build --preset build-debug-gcc-snapshot
  ```

### Android arm64-v8a (`android-arm64-v8a`)
- **Description:** Configures the build for Android arm64-v8a JNI shared library.
- **Toolchain:** Android NDK toolchain at `$env{HOME}/android-sdk/ndk/30.0.14904198/build/cmake/android.toolchain.cmake`
- **Binary Directory:** `build-android-arm64-v8a`
- **Configuration Command:**
  ```bash
  cmake --preset android-arm64-v8a
  ```
- **Build Preset:** `build-android-arm64-v8a`
- **Build Command:**
  ```bash
  cmake --build --preset build-android-arm64-v8a
  ```

### Android x86_64 (`android-x86_64`)
- **Description:** Configures the build for Android x86_64 JNI shared library.
- **Toolchain:** Android NDK toolchain at `$env{HOME}/android-sdk/ndk/30.0.14904198/build/cmake/android.toolchain.cmake`
- **Binary Directory:** `build-android-x86_64`
- **Configuration Command:**
  ```bash
  cmake --preset android-x86_64
  ```
- **Build Preset:** `build-android-x86_64`
- **Build Command:**
  ```bash
  cmake --build --preset build-android-x86_64
  ```

---

## 3. General Build Rules

- **Do not modify `CMakePresets.json`** unless explicitly requested by the user or required to add a new build configuration.
- **Clean builds:** If you need to clean or rebuild from scratch, delete the corresponding binary directory (e.g., `rm -rf build`) and re-run the configuration preset.
- **Build Outputs:** Always ensure your build output directory is ignored by Git (already covered in `.gitignore` for `build*`).
