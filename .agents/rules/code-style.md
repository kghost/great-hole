---
trigger: model_decision
description: "C++ coding style guidelines for the GreatHole project"
---

# GreatHole C++ Code Style Guidelines

All C++ source (`.cpp`) and header (`.hpp`) files in the GreatHole repository must adhere strictly to these guidelines to ensure consistency, readability, and modern C++ practices.

*Except for* external dependencies in `external/`.

## 1. Naming Conventions

- **Classes, Structs, Enums, Type aliases (`using`):** `PascalCase`
  - *Example:* `class ProcessTree`, `struct MetricEntry`, `using PidType = int;`
- **Methods and Functions:** `PascalCase`
  - *Example:* `void MoveCursorAndDraw(ssize_t offset);`, `std::string GetTabName() const;`
- **Member Variables (Private or Protected):** `_PascalCase` (Preceded by a single underscore and starting with an uppercase letter)
  - *Example:* `GreatHoleInterface& _Interface;`, `std::list<Row> _Rows;`
- **Member Variables (Public):** `PascalCase` (No underscore prefix)
  - *Example:* `ProcessTree& Tree;`, `backend::process::ProcessMetrics Metrics;`
- **Local Variables and Function Parameters:** `camelCase`
  - *Example:* `ssize_t offset`, `backend::process::Process& process`, `auto newCapacity = ...;`
- **Constants (Global / `constexpr` / `static const`):** `kPascalCase` or `ALL_CAPS` (for legacy/sys-like constants)
  - *Example:* `constexpr const char* kProcessTreeTabName = "Process Tree";`
  - *Example:* `static const std::filesystem::path PROC_PATH;`

## 2. Formatting & Layout (Clang-Format)

We use `.clang-format` based on `LLVM` style with specific customizations. Key rules:
- **Indentation:** 2 spaces (no tabs).
- **Column Limit:** 120 characters.
- **Pointer & Reference Alignment:** Left-aligned to the type, not the variable.
  - *Correct:* `ProcessTree& tree`, `std::unique_ptr<Column> column`
  - *Incorrect:* `ProcessTree &tree`, `std::unique_ptr<Column> *column`
- **Braces:** Always insert braces for control flow statements (`InsertBraces: true`).
  - *Correct:*
    ```cpp
    if (condition) {
      doSomething();
    }
    ```
    *Incorrect:*
    ```cpp
    if (condition)
      doSomething();
    ```

## 3. Header Rules

- **Header Guard:** Always use `#pragma once` at the top of header files.
- **Includes Ordering & Spacing:**
  - Group `#include` lines and separate them with a single blank line:
    1. The corresponding header associated with the cpp file (in `.cpp` files, the header with same file name as cpp file).
    2. C++ Standard Library headers in alphabetical order.
    3. External dependencies (e.g. `<ftxui/...>`) in alphabetical order.
    4. Internal project headers using relative paths (e.g. `"../../backend/process/Process.hpp"`).
  - *Example:*
    ```cpp
    #include "ProcessTree.hpp"

    #include <algorithm>
    #include <memory>
    #include <ranges>

    #include <ftxui/dom/table.hpp>

    #include "../../backend/process/ProcessMetrics.hpp"
    ```

## 4. Modern C++ & Memory Management

- **C++ Version:** C++23. Use modern features like `<ranges>`, `std::views`, `std::optional`, `std::reference_wrapper`, etc.
- **Constructors:**
  - Use `explicit` on all constructors that can be called with a single argument to prevent implicit conversions.
  - *Example:* `explicit Process(ProcessManager& manager, PidType pid);`
- **Virtual Functions:** Always use the `override` keyword when overriding a virtual method from a base class. Use `virtual` only for the base declaration.
- **Smart Pointers:** Prefer standard ownership patterns:
  - Use `std::unique_ptr` for exclusive ownership.
  - Use `std::shared_ptr` for shared ownership.
  - Pass raw references/pointers or `std::reference_wrapper` for non-owning access.
- **Delete Copy/Move Constructors:** Explicitly mark unsupported copy/move operations as `= delete`.
  - *Example:*
    ```cpp
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    ```
- Prefer std::array over C-style array.
- **Avoid pointers**
  - Use std::optional<std::reference_wrapper<T>> to replace nullable pointer
  - Use T& to replace non-nullable pointer
  - Unless required by platform API or third-party API, try avoid use pointers.
- **Null Pointers:** Use `nullptr` instead of `NULL` or `0` for pointer values.

## 5. Namespaces

- Prefer standard nested namespace declarations.
- Comment the closing brace of a namespace with `// namespace ...`.
- *Example:*
  ```cpp
  namespace frontend::ftxui {

  // ... implementation ...

  } // namespace frontend::ftxui
  ```
