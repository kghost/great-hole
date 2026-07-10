#pragma once

#include <string>

#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "Resolver.hpp"

namespace gh {

class ResolverServicePort final : public ResolverPort {
public:
  explicit ResolverServicePort(std::string const& portStr);
  ~ResolverServicePort() override = default;

  ResolverServicePort(const ResolverServicePort&) = delete;
  auto operator=(const ResolverServicePort&) -> ResolverServicePort& = delete;
  ResolverServicePort(ResolverServicePort&&) = delete;
  auto operator=(ResolverServicePort&&) -> ResolverServicePort& = delete;

  auto GetResolverResult() const -> uint16_t override;

protected:
  auto GetName() const -> std::string override;
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  std::string _PortStr;
  uint16_t _Port = 0;
};

} // namespace gh
