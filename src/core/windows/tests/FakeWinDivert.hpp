#pragma once

#include <windivert.h>
#include <windows.h>

#include <functional>
#include <vector>

namespace gh::test {

class FakeWinDivertController {
public:
  virtual ~FakeWinDivertController() = default;

  virtual void
  SetOpenCallback(std::function<void(HANDLE handle, const char* filter, WINDIVERT_LAYER layer)> callback) = 0;

  virtual void SetCloseCallback(std::function<void(HANDLE handle)> callback) = 0;

  virtual void
  SetSendCallback(std::function<void(HANDLE handle, const std::vector<uint8_t>& packet, const WINDIVERT_ADDRESS& addr)>
                      callback) = 0;

  virtual void PushRecvPacket(HANDLE handle, const std::vector<uint8_t>& packet, const WINDIVERT_ADDRESS& addr) = 0;

  virtual void Reset() = 0;
};

auto GetFakeWinDivertController() -> FakeWinDivertController&;

} // namespace gh::test
