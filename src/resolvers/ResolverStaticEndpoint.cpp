#include "ResolverStaticEndpoint.hpp"

namespace gh {

ResolverStaticEndpoint::ResolverStaticEndpoint(boost::asio::ip::udp::endpoint const& endpoint) : _Endpoint(endpoint) {}
boost::asio::ip::udp::endpoint ResolverStaticEndpoint::GetResolverResult() const { return _Endpoint; }
Omni::Fiber::Coroutine<ErrorCode> ResolverStaticEndpoint::DoStart() { co_return ErrorCode{}; }
Omni::Fiber::Coroutine<ErrorCode> ResolverStaticEndpoint::DoGracefulStop() { co_return ErrorCode{}; }

} // namespace gh
