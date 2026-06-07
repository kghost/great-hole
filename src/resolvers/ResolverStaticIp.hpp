#pragma once

#include <string>

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "Resolver.hpp"

namespace gh {

class ResolverStaticIp final : public ResolverIp {
public:
  explicit ResolverStaticIp(const boost::asio::ip::address_v6& address);
  ~ResolverStaticIp() override = default;

  ResolverStaticIp(const ResolverStaticIp&) = delete;
  ResolverStaticIp& operator=(const ResolverStaticIp&) = delete;
  ResolverStaticIp(ResolverStaticIp&&) = delete;
  ResolverStaticIp& operator=(ResolverStaticIp&&) = delete;

  boost::asio::ip::address_v6 GetResolverResult() const override;

protected:
  std::string GetName() const override;
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  boost::asio::ip::address_v6 _Address;
};

} // namespace gh
