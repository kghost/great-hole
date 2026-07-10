#pragma once

#include <string>

#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "Resolver.hpp"

namespace gh {

class ResolverNumberPort final : public ResolverPort {
public:
  explicit ResolverNumberPort(uint16_t port);
  ~ResolverNumberPort() override = default;

  ResolverNumberPort(const ResolverNumberPort&) = delete;
  auto operator=(const ResolverNumberPort&) -> ResolverNumberPort& = delete;
  ResolverNumberPort(ResolverNumberPort&&) = delete;
  auto operator=(ResolverNumberPort&&) -> ResolverNumberPort& = delete;

  auto GetResolverResult() const -> uint16_t override;

protected:
  auto GetName() const -> std::string override;
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  uint16_t _Port = 0;
};

} // namespace gh
