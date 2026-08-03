#pragma once

#include <optional>
#include <string>
#include <windows.h>

#include "AutoHandle.hpp"
#include "Interface.hpp"

namespace gh {

[[nodiscard]] auto GetProcessSequence(HANDLE hProcess) -> std::optional<Interface::ProcessSequence>;
[[nodiscard]] auto GetParentProcessId(HANDLE hProcess) -> std::optional<Interface::ProcessId>;
[[nodiscard]] auto GetProcessPath(HANDLE hProcess) -> std::optional<std::string>;

[[nodiscard]] auto GetProcessSequence(Interface::ProcessId pid) -> std::optional<Interface::ProcessSequence>;

template <typename Function> auto WithProcessHandle(Interface::ProcessId pid, Function&& function) -> decltype(auto) {
  AutoHandle hProcess{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
  if (!hProcess.IsValid()) {
    hProcess.Reset(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid));
  }
  if (hProcess.IsValid()) {
    return std::forward<Function>(function)(hProcess.Get());
  } else {
    return std::forward<Function>(function)(std::nullopt);
  }
}

} // namespace gh
