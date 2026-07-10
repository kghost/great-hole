#pragma once

#include <string>
#include <vector>

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "Resolver.hpp"

namespace gh {

class ResolverDnsService final : public ResolverEndpoint {
public:
  explicit ResolverDnsService(const std::string& serviceName, ResolveFor& target);
  ~ResolverDnsService() override = default;

  ResolverDnsService(const ResolverDnsService&) = delete;
  auto operator=(const ResolverDnsService&) -> ResolverDnsService& = delete;
  ResolverDnsService(ResolverDnsService&&) = delete;
  auto operator=(ResolverDnsService&&) -> ResolverDnsService& = delete;

  auto GetResolverResult() const -> boost::asio::ip::udp::endpoint override;

protected:
  auto GetName() const -> std::string override { return "ResolverDnsService"; }
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  const std::string _ServiceName;
  ResolveFor& _Target;
  std::vector<boost::asio::ip::udp::endpoint> _Endpoints;
};

} // namespace gh
