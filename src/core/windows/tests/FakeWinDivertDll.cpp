#include <windivert.h>
#include <windows.h>
#include <winsock2.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "FakeWinDivert.hpp"

namespace gh::test {

struct PendingRead {
  HANDLE handle;
  WINDIVERT_ADDRESS* addr;
  UINT* addrLen;
  LPOVERLAPPED overlapped;
};

class FakeWinDivertControllerImpl : public FakeWinDivertController {
public:
  void
  SetOpenCallback(std::function<void(HANDLE handle, const char* filter, WINDIVERT_LAYER layer)> callback) override {
    std::lock_guard<std::mutex> lock(_Mutex);
    _OpenCallback = callback;
  }

  void SetCloseCallback(std::function<void(HANDLE handle)> callback) override {
    std::lock_guard<std::mutex> lock(_Mutex);
    _CloseCallback = callback;
  }

  void SetSendCallback(
      std::function<void(HANDLE handle, const std::vector<uint8_t>& packet, const WINDIVERT_ADDRESS& addr)> callback)
      override {
    std::lock_guard<std::mutex> lock(_Mutex);
    _SendCallback = callback;
  }

  void PushRecvPacket(HANDLE handle, const std::vector<uint8_t>& packet, const WINDIVERT_ADDRESS& addr) override {
    std::lock_guard<std::mutex> lock(_Mutex);

    CleanUpCancelledReads();

    auto it = std::find_if(_PendingReads.begin(), _PendingReads.end(),
                           [handle](const PendingRead& pr) { return pr.handle == handle; });

    if (it != _PendingReads.end()) {
      if (it->addr != nullptr) {
        *it->addr = addr;
        if (it->addrLen != nullptr) {
          *it->addrLen = sizeof(WINDIVERT_ADDRESS);
        }
      }

      auto pipeIt = _HandleMap.find(handle);
      if (pipeIt != _HandleMap.end()) {
        DWORD written = 0;
        WriteFile(pipeIt->second, packet.data(), static_cast<DWORD>(packet.size()), &written, nullptr);
      }

      _PendingReads.erase(it);
    } else {
      _BufferedAddresses[handle].push_back(addr);
      auto pipeIt = _HandleMap.find(handle);
      if (pipeIt != _HandleMap.end()) {
        DWORD written = 0;
        WriteFile(pipeIt->second, packet.data(), static_cast<DWORD>(packet.size()), &written, nullptr);
      }
    }
  }

  void Reset() override {
    std::lock_guard<std::mutex> lock(_Mutex);
    _OpenCallback = nullptr;
    _CloseCallback = nullptr;
    _SendCallback = nullptr;
    _BufferedAddresses.clear();
    _PendingReads.clear();
  }

  auto Open(const char* filter, WINDIVERT_LAYER layer, INT16 priority, UINT64 flags) -> HANDLE {
    std::lock_guard<std::mutex> lock(_Mutex);

    static std::atomic<uint32_t> pipeId{0};
    std::wstring pipeName = std::format(L"\\\\.\\pipe\\FakeWinDivert_{}_{}", GetCurrentProcessId(), ++pipeId);

    HANDLE serverPipe = CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                         PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 1024 * 1024,
                                         1024 * 1024, 0, nullptr);

    if (serverPipe == INVALID_HANDLE_VALUE) {
      return INVALID_HANDLE_VALUE;
    }

    HANDLE clientPipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED, nullptr);

    if (clientPipe == INVALID_HANDLE_VALUE) {
      CloseHandle(serverPipe);
      return INVALID_HANDLE_VALUE;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(clientPipe, &mode, nullptr, nullptr);

    OVERLAPPED connectOverlapped{};
    connectOverlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (connectOverlapped.hEvent == nullptr) {
      CloseHandle(clientPipe);
      CloseHandle(serverPipe);
      return INVALID_HANDLE_VALUE;
    }

    if (ConnectNamedPipe(serverPipe, &connectOverlapped) == FALSE) {
      DWORD err = GetLastError();
      if (err == ERROR_PIPE_CONNECTED) {
        SetEvent(connectOverlapped.hEvent);
      } else if (err != ERROR_IO_PENDING) {
        CloseHandle(connectOverlapped.hEvent);
        CloseHandle(clientPipe);
        CloseHandle(serverPipe);
        return INVALID_HANDLE_VALUE;
      }
    }

    WaitForSingleObject(connectOverlapped.hEvent, INFINITE);
    CloseHandle(connectOverlapped.hEvent);

    _HandleMap[clientPipe] = serverPipe;

    if (_OpenCallback) {
      _OpenCallback(clientPipe, filter, layer);
    }

    return clientPipe;
  }

  auto Close(HANDLE handle) -> BOOL {
    std::lock_guard<std::mutex> lock(_Mutex);

    auto it = _HandleMap.find(handle);
    if (it != _HandleMap.end()) {
      CloseHandle(it->second);
      _HandleMap.erase(it);
    }

    CloseHandle(handle);

    _PendingReads.erase(std::remove_if(_PendingReads.begin(), _PendingReads.end(),
                                       [handle](const PendingRead& pr) { return pr.handle == handle; }),
                        _PendingReads.end());

    _BufferedAddresses.erase(handle);

    if (_CloseCallback) {
      _CloseCallback(handle);
    }

    return TRUE;
  }

  auto RecvEx(HANDLE handle, void* packet, UINT packetLen, UINT* recvLen, UINT64 flags, WINDIVERT_ADDRESS* addr,
              UINT* addrLen, LPOVERLAPPED overlapped) -> BOOL {
    std::lock_guard<std::mutex> lock(_Mutex);

    CleanUpCancelledReads();

    if (overlapped != nullptr) {
      _PendingReads.push_back({handle, addr, addrLen, overlapped});

      auto& buf = _BufferedAddresses[handle];
      if (!buf.empty()) {
        if (addr != nullptr) {
          *addr = buf.front();
          if (addrLen != nullptr) {
            *addrLen = sizeof(WINDIVERT_ADDRESS);
          }
        }
        buf.pop_front();
      }

      DWORD transferred = 0;
      BOOL ret = ReadFile(handle, packet, packetLen, &transferred, overlapped);
      if (ret == TRUE) {
        if (recvLen != nullptr) {
          *recvLen = transferred;
        }
        _PendingReads.pop_back();
      } else {
        if (GetLastError() != ERROR_IO_PENDING) {
          _PendingReads.pop_back();
        }
      }
      return ret;
    } else {
      auto& buf = _BufferedAddresses[handle];
      if (!buf.empty()) {
        if (addr != nullptr) {
          *addr = buf.front();
          if (addrLen != nullptr) {
            *addrLen = sizeof(WINDIVERT_ADDRESS);
          }
        }
        buf.pop_front();
      }

      DWORD transferred = 0;
      BOOL ret = ReadFile(handle, packet, packetLen, &transferred, nullptr);
      if (ret == TRUE && recvLen != nullptr) {
        *recvLen = transferred;
      }
      return ret;
    }
  }

  auto SendEx(HANDLE handle, const void* packet, UINT packetLen, UINT* sendLen, UINT64 flags,
              const WINDIVERT_ADDRESS* addr, UINT addrLen, LPOVERLAPPED overlapped) -> BOOL {
    std::lock_guard<std::mutex> lock(_Mutex);

    std::vector<uint8_t> data(static_cast<const uint8_t*>(packet), static_cast<const uint8_t*>(packet) + packetLen);

    WINDIVERT_ADDRESS address{};
    if (addr != nullptr && addrLen >= sizeof(WINDIVERT_ADDRESS)) {
      address = *addr;
    }

    if (_SendCallback) {
      _SendCallback(handle, data, address);
    }

    if (sendLen != nullptr) {
      *sendLen = packetLen;
    }

    return TRUE;
  }

private:
  void CleanUpCancelledReads() {
    _PendingReads.erase(std::remove_if(_PendingReads.begin(), _PendingReads.end(),
                                       [](const PendingRead& pr) { return pr.overlapped->Internal != 0x103; }),
                        _PendingReads.end());
  }

  std::mutex _Mutex;
  std::function<void(HANDLE handle, const char* filter, WINDIVERT_LAYER layer)> _OpenCallback;
  std::function<void(HANDLE handle)> _CloseCallback;
  std::function<void(HANDLE handle, const std::vector<uint8_t>& packet, const WINDIVERT_ADDRESS& addr)> _SendCallback;

  std::map<HANDLE, HANDLE> _HandleMap;
  std::map<HANDLE, std::deque<WINDIVERT_ADDRESS>> _BufferedAddresses;
  std::vector<PendingRead> _PendingReads;
};

FakeWinDivertControllerImpl g_Controller;

} // namespace gh::test

extern "C" {

__declspec(dllexport) auto WinDivertOpen(const char* filter, WINDIVERT_LAYER layer, INT16 priority, UINT64 flags)
    -> HANDLE {
  return gh::test::g_Controller.Open(filter, layer, priority, flags);
}

__declspec(dllexport) auto WinDivertClose(HANDLE handle) -> BOOL { return gh::test::g_Controller.Close(handle); }

__declspec(dllexport) auto WinDivertRecvEx(HANDLE handle, void* packet, UINT packetLen, UINT* recvLen, UINT64 flags,
                                           WINDIVERT_ADDRESS* addr, UINT* addrLen, LPOVERLAPPED overlapped) -> BOOL {
  return gh::test::g_Controller.RecvEx(handle, packet, packetLen, recvLen, flags, addr, addrLen, overlapped);
}

__declspec(dllexport) auto WinDivertSendEx(HANDLE handle, const void* packet, UINT packetLen, UINT* sendLen,
                                           UINT64 flags, const WINDIVERT_ADDRESS* addr, UINT addrLen,
                                           LPOVERLAPPED overlapped) -> BOOL {
  return gh::test::g_Controller.SendEx(handle, packet, packetLen, sendLen, flags, addr, addrLen, overlapped);
}

__declspec(dllexport) void WinDivertHelperHtonIPv6Address(const UINT* inAddr, UINT* outAddr) {
  if (inAddr != nullptr && outAddr != nullptr) {
    outAddr[0] = htonl(inAddr[0]);
    outAddr[1] = htonl(inAddr[1]);
    outAddr[2] = htonl(inAddr[2]);
    outAddr[3] = htonl(inAddr[3]);
  }
}

__declspec(dllexport) auto GetFakeWinDivertControllerPtr() -> gh::test::FakeWinDivertController* {
  return &gh::test::g_Controller;
}
}
