#pragma once

#include "Coroutine.hpp"
#include "Packet.hpp"

namespace gh {

class DeferredPacketInjector {
public:
  explicit DeferredPacketInjector() = default;
  virtual ~DeferredPacketInjector() = default;

  DeferredPacketInjector(const DeferredPacketInjector&) = delete;
  auto operator=(const DeferredPacketInjector&) -> DeferredPacketInjector& = delete;
  DeferredPacketInjector(DeferredPacketInjector&&) = delete;
  auto operator=(DeferredPacketInjector&&) -> DeferredPacketInjector& = delete;

  virtual auto Inject(Packet&& packet) -> Omni::Fiber::Coroutine<void> = 0;
};

} // namespace gh
