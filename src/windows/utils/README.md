# Windows Utilities Module (`gh::utils`)

The Windows Utilities module provides shared helper classes and utility functions used across GreatHole's Windows sub-modules.

## Features

- **`AutoHandle` (`UniqueHandle`)**: RAII wrapper class for Windows `HANDLE` objects (`CloseHandle` cleanup).
- **Process Utilities (`Process.hpp`)**: Safe wrappers for querying process details, sequence numbers, parent process IDs, and process image paths.
- **String Utilities (`Strings.hpp`)**: Conversion utilities between UTF-8 `std::string` and UTF-16 `std::wstring`.

## Usage Example

### `AutoHandle`

```cpp
#include "AutoHandle.hpp"

void ProcessEvent() {
  gh::AutoHandle hEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (hEvent) {
    // Use hEvent.Get()
  } // Automatically calls CloseHandle when exiting scope
}
```
