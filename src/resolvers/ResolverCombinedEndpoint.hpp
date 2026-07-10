#pragma once

#include <memory>

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "Resolver.hpp"

namespace gh {

class ResolverCombinedEndpoint final : public ResolverEndpoint {
public:
  explicit ResolverCombinedEndpoint(std::shared_ptr<ResolverIp> ipResolver, std::shared_ptr<ResolverPort> portResolver);
  ~ResolverCombinedEndpoint() override = default;

  ResolverCombinedEndpoint(const ResolverCombinedEndpoint&) = delete;
  auto operator=(const ResolverCombinedEndpoint&) -> ResolverCombinedEndpoint& = delete;
  ResolverCombinedEndpoint(ResolverCombinedEndpoint&&) = delete;
  auto operator=(ResolverCombinedEndpoint&&) -> ResolverCombinedEndpoint& = delete;

  auto GetResolverResult() const -> boost::asio::ip::udp::endpoint override;

protected:
  auto GetName() const -> std::string override;
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  std::shared_ptr<ResolverIp> _IpResolver;
  std::shared_ptr<ResolverPort> _PortResolver;
  boost::asio::ip::udp::endpoint _Endpoint;
};

} // namespace gh
