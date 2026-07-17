#pragma once

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "Resolver.hpp"

namespace gh {

class ResolverStaticEndpoint final : public ResolverEndpoint {
public:
  explicit ResolverStaticEndpoint(boost::asio::ip::udp::endpoint const& endpoint);
  ~ResolverStaticEndpoint() override = default;

  ResolverStaticEndpoint(const ResolverStaticEndpoint&) = delete;
  auto operator=(const ResolverStaticEndpoint&) -> ResolverStaticEndpoint& = delete;
  ResolverStaticEndpoint(ResolverStaticEndpoint&&) = delete;
  auto operator=(ResolverStaticEndpoint&&) -> ResolverStaticEndpoint& = delete;

  auto GetResolverResult() const -> boost::asio::ip::udp::endpoint override;
  auto GetName() const -> std::string override;

protected:
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  boost::asio::ip::udp::endpoint _Endpoint;
};

} // namespace gh
