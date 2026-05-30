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
  ResolverIpDns& operator=(const ResolverIpDns&) = delete;
  ResolverIpDns(ResolverIpDns&&) = delete;
  ResolverIpDns& operator=(ResolverIpDns&&) = delete;

  boost::asio::ip::address GetResolverResult() const override;

protected:
  std::string GetName() const override { return "ResolverIpDns"; }
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  boost::asio::any_io_executor _Executor;
  std::string _Host;
  std::vector<boost::asio::ip::address> _Addresses;
};

} // namespace gh
