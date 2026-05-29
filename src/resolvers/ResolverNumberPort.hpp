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
  ResolverNumberPort& operator=(const ResolverNumberPort&) = delete;
  ResolverNumberPort(ResolverNumberPort&&) = delete;
  ResolverNumberPort& operator=(ResolverNumberPort&&) = delete;

  uint16_t GetPort() const override;

protected:
  std::string GetName() const override;
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  uint16_t _Port = 0;
};

} // namespace gh
