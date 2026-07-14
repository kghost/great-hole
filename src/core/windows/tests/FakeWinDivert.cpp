#include "FakeWinDivert.hpp"

#include <stdexcept>
#include <windows.h>

namespace gh::test {

auto GetFakeWinDivertController() -> FakeWinDivertController& {
  HMODULE h = GetModuleHandleW(L"WinDivert.dll");
  if (h == nullptr) {
    h = LoadLibraryW(L"WinDivert.dll");
  }
  if (h == nullptr) {
    throw std::runtime_error("Failed to load WinDivert.dll");
  }
  auto getPtr = reinterpret_cast<FakeWinDivertController* (*)()>(GetProcAddress(h, "GetFakeWinDivertControllerPtr"));
  if (getPtr == nullptr) {
    throw std::runtime_error("Failed to find GetFakeWinDivertControllerPtr in WinDivert.dll");
  }
  return *getPtr();
}

} // namespace gh::test
