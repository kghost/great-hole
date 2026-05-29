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
  ResolverDnsService& operator=(const ResolverDnsService&) = delete;
  ResolverDnsService(ResolverDnsService&&) = delete;
  ResolverDnsService& operator=(ResolverDnsService&&) = delete;

  boost::asio::ip::udp::endpoint GetEndpoint() const override;

protected:
  std::string GetName() const override { return "ResolverDnsService"; }
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  const std::string _ServiceName;
  ResolveFor& _Target;
  std::vector<boost::asio::ip::udp::endpoint> _Endpoints;
};

} // namespace gh
