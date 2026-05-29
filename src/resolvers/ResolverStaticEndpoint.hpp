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
  ResolverStaticEndpoint& operator=(const ResolverStaticEndpoint&) = delete;
  ResolverStaticEndpoint(ResolverStaticEndpoint&&) = delete;
  ResolverStaticEndpoint& operator=(ResolverStaticEndpoint&&) = delete;

  boost::asio::ip::udp::endpoint GetEndpoint() const override;

protected:
  std::string GetName() const override { return "ResolverStaticEndpoint"; }
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  boost::asio::ip::udp::endpoint _Endpoint;
};

} // namespace gh
