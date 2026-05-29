#include "ResolverStaticIp.hpp"

#include <boost/system/system_error.hpp>

namespace gh {

ResolverStaticIp::ResolverStaticIp(const boost::asio::ip::address& address) : _Address(address) {}

boost::asio::ip::address ResolverStaticIp::GetAddress() const { return _Address; }

Omni::Fiber::Coroutine<ErrorCode> ResolverStaticIp::DoStart() { co_return ErrorCode{}; }

Omni::Fiber::Coroutine<ErrorCode> ResolverStaticIp::DoGracefulStop() { co_return ErrorCode{}; }

} // namespace gh
