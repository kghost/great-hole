#include "ResolverStaticEndpoint.hpp"

#include <boost/lexical_cast.hpp>

namespace gh {

ResolverStaticEndpoint::ResolverStaticEndpoint(boost::asio::ip::udp::endpoint const& endpoint) : _Endpoint(endpoint) {}
auto ResolverStaticEndpoint::GetResolverResult() const -> boost::asio::ip::udp::endpoint { return _Endpoint; }
auto ResolverStaticEndpoint::GetName() const -> std::string {
  return std::format("ResolverStaticEndpoint:{}", boost::lexical_cast<std::string>(_Endpoint));
}
auto ResolverStaticEndpoint::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> { co_return ErrorCode{}; }
auto ResolverStaticEndpoint::DoWork() -> Omni::Fiber::Coroutine<void> {
  _ResolveError = ErrorCode{};
  co_return;
}
auto ResolverStaticEndpoint::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> { co_return ErrorCode{}; }

} // namespace gh
