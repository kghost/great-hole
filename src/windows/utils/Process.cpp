#include "Process.hpp"

#include <array>
#include <optional>
#include <windows.h>
#include <winternl.h>

#include "AutoHandle.hpp"
#include "InterfaceWin32.hpp"
#include "Strings.hpp"

namespace gh {

auto GetProcessSequence(HANDLE hProcess) -> std::optional<Interface::ProcessSequence> {
  using pNtQueryInformationProcess =
      NTSTATUS(NTAPI*)(HANDLE ProcessHandle, ULONG ProcessInformationClass, PVOID ProcessInformation,
                       ULONG ProcessInformationLength, PULONG ReturnLength);

  static auto ntQueryInfo = []() -> pNtQueryInformationProcess {
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (hNtdll != nullptr) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      return reinterpret_cast<pNtQueryInformationProcess>(GetProcAddress(hNtdll, "NtQueryInformationProcess"));
    }
    return nullptr;
  }();

  constexpr ULONG kProcessSequenceClass = 92;
  ULONGLONG seq = 0;
  ULONG returnLen = 0;
  NTSTATUS status = ntQueryInfo(hProcess, kProcessSequenceClass, &seq, sizeof(seq), &returnLen);
  if (status == 0) {
    return seq;
  }

  return std::nullopt;
}

auto GetProcessSequence(Interface::ProcessId pid) -> std::optional<Interface::ProcessSequence> {
  AutoHandle hProcess{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
  if (!hProcess) {
    hProcess.Reset(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid));
  }
  if (!hProcess) {
    return std::nullopt;
  }

  return GetProcessSequence(hProcess.Get());
}

auto GetParentProcessId(HANDLE hProcess) -> std::optional<Interface::ProcessId> {
  using pNtQueryInformationProcess =
      NTSTATUS(NTAPI*)(HANDLE ProcessHandle, ULONG ProcessInformationClass, PVOID ProcessInformation,
                       ULONG ProcessInformationLength, PULONG ReturnLength);

  static auto ntQueryInfo = []() -> pNtQueryInformationProcess {
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (hNtdll != nullptr) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      return reinterpret_cast<pNtQueryInformationProcess>(GetProcAddress(hNtdll, "NtQueryInformationProcess"));
    }
    return nullptr;
  }();

  struct PROCESS_BASIC_INFORMATION {
    PVOID Reserved1;
    PVOID PebBaseAddress;
    std::array<PVOID, 2> Reserved2;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR InheritedFromUniqueProcessId;
  };
  PROCESS_BASIC_INFORMATION pbi{};
  ULONG returnLen = 0;
  NTSTATUS status = ntQueryInfo(hProcess, 0 /* ProcessBasicInformation */, &pbi, sizeof(pbi), &returnLen);
  if (status == 0 && pbi.InheritedFromUniqueProcessId != 0) {
    return static_cast<Interface::ProcessId>(pbi.InheritedFromUniqueProcessId);
  }
  return std::nullopt;
}

auto GetProcessPath(HANDLE hProcess) -> std::optional<std::string> {
  std::array<wchar_t, MAX_PATH * 2> path{0};
  auto size = static_cast<DWORD>(path.size());
  if (QueryFullProcessImageNameW(hProcess, 0, path.data(), &size) != FALSE) {
    return ToString(path.data());
  }
  return std::nullopt;
}

} // namespace gh
