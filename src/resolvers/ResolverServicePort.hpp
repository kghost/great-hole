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
  ResolverServicePort& operator=(const ResolverServicePort&) = delete;
  ResolverServicePort(ResolverServicePort&&) = delete;
  ResolverServicePort& operator=(ResolverServicePort&&) = delete;

  uint16_t GetPort() const override;

protected:
  std::string GetName() const override;
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  std::string _PortStr;
  uint16_t _Port = 0;
};

} // namespace gh
