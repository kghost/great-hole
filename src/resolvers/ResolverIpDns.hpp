#pragma once

#include <string>
#include <vector>

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "Resolver.hpp"

namespace gh {

class ResolverIpDns final : public ResolverIp {
public:
  explicit ResolverIpDns(boost::asio::any_io_executor executor, std::string const& host);
  ~ResolverIpDns() override = default;

  ResolverIpDns(const ResolverIpDns&) = delete;
  auto operator=(const ResolverIpDns&) -> ResolverIpDns& = delete;
  ResolverIpDns(ResolverIpDns&&) = delete;
  auto operator=(ResolverIpDns&&) -> ResolverIpDns& = delete;

  auto GetResolverResult() const -> boost::asio::ip::address_v6 override;

protected:
  auto GetName() const -> std::string override;
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  boost::asio::any_io_executor _Executor;
  std::string _Host;
  std::vector<boost::asio::ip::address_v6> _Addresses;
};

} // namespace gh
