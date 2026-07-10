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
  auto operator=(const ResolverStaticIp&) -> ResolverStaticIp& = delete;
  ResolverStaticIp(ResolverStaticIp&&) = delete;
  auto operator=(ResolverStaticIp&&) -> ResolverStaticIp& = delete;

  auto GetResolverResult() const -> boost::asio::ip::address_v6 override;

protected:
  auto GetName() const -> std::string override;
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  boost::asio::ip::address_v6 _Address;
};

} // namespace gh
