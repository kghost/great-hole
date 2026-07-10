#include "ResolverStaticIp.hpp"

#include <boost/lexical_cast.hpp>
#include <boost/system/system_error.hpp>

namespace gh {

ResolverStaticIp::ResolverStaticIp(const boost::asio::ip::address_v6& address) : _Address(address) {}

auto ResolverStaticIp::GetName() const -> std::string {
  return "ResolverStaticIp:" + boost::lexical_cast<std::string>(_Address);
}

auto ResolverStaticIp::GetResolverResult() const -> boost::asio::ip::address_v6 { return _Address; }
auto ResolverStaticIp::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> { co_return ErrorCode{}; }
auto ResolverStaticIp::DoWork() -> Omni::Fiber::Coroutine<void> {
  _ResolveError = ErrorCode{};
  co_return;
}
auto ResolverStaticIp::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> { co_return ErrorCode{}; }

} // namespace gh
