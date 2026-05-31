#include "ResolverStaticEndpoint.hpp"

#include <boost/lexical_cast.hpp>

namespace gh {

ResolverStaticEndpoint::ResolverStaticEndpoint(boost::asio::ip::udp::endpoint const& endpoint) : _Endpoint(endpoint) {}
boost::asio::ip::udp::endpoint ResolverStaticEndpoint::GetResolverResult() const { return _Endpoint; }
std::string ResolverStaticEndpoint::GetName() const {
  return std::format("ResolverStaticEndpoint:{}", boost::lexical_cast<std::string>(_Endpoint));
}
Omni::Fiber::Coroutine<ErrorCode> ResolverStaticEndpoint::DoStart() { co_return ErrorCode{}; }
Omni::Fiber::Coroutine<ErrorCode> ResolverStaticEndpoint::DoGracefulStop() { co_return ErrorCode{}; }

} // namespace gh
