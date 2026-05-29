#pragma once

#include <string>

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "Resolver.hpp"

namespace gh {

class ResolverStaticIp final : public ResolverIp {
public:
  explicit ResolverStaticIp(const boost::asio::ip::address& address);
  ~ResolverStaticIp() override = default;

  ResolverStaticIp(const ResolverStaticIp&) = delete;
  ResolverStaticIp& operator=(const ResolverStaticIp&) = delete;
  ResolverStaticIp(ResolverStaticIp&&) = delete;
  ResolverStaticIp& operator=(ResolverStaticIp&&) = delete;

  boost::asio::ip::address GetAddress() const override;

protected:
  std::string GetName() const override { return "ResolverStaticIp"; }
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  boost::asio::ip::address _Address;
};

} // namespace gh
