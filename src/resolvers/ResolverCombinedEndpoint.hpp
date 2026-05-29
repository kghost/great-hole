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
  ResolverCombinedEndpoint& operator=(const ResolverCombinedEndpoint&) = delete;
  ResolverCombinedEndpoint(ResolverCombinedEndpoint&&) = delete;
  ResolverCombinedEndpoint& operator=(ResolverCombinedEndpoint&&) = delete;

  boost::asio::ip::udp::endpoint GetEndpoint() const override;

protected:
  std::string GetName() const override;
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  std::shared_ptr<ResolverIp> _IpResolver;
  std::shared_ptr<ResolverPort> _PortResolver;
  boost::asio::ip::udp::endpoint _Endpoint;
};

} // namespace gh
