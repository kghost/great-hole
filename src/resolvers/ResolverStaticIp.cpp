#include "ResolverStaticIp.hpp"

#include <boost/lexical_cast.hpp>
#include <boost/system/system_error.hpp>

namespace gh {

ResolverStaticIp::ResolverStaticIp(const boost::asio::ip::address& address) : _Address(address) {}

std::string ResolverStaticIp::GetName() const {
  return "ResolverStaticIp:" + boost::lexical_cast<std::string>(_Address);
}

boost::asio::ip::address ResolverStaticIp::GetResolverResult() const { return _Address; }
Omni::Fiber::Coroutine<ErrorCode> ResolverStaticIp::DoStart() { co_return ErrorCode{}; }
Omni::Fiber::Coroutine<ErrorCode> ResolverStaticIp::DoGracefulStop() { co_return ErrorCode{}; }

} // namespace gh
