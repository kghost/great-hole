#include "ResolverCombinedEndpoint.hpp"

#include "GetCurrentFiber.hpp"

namespace gh {

ResolverCombinedEndpoint::ResolverCombinedEndpoint(std::shared_ptr<ResolverIp> ipResolver,
                                                   std::shared_ptr<ResolverPort> portResolver)
    : _IpResolver(ipResolver), _PortResolver(portResolver) {}

boost::asio::ip::udp::endpoint ResolverCombinedEndpoint::GetEndpoint() const { return _Endpoint; }

Omni::Fiber::Coroutine<ErrorCode> ResolverCombinedEndpoint::DoStart() {
  auto errIp = co_await _IpResolver->Start();
  if (errIp) {
    co_return errIp;
  }
  auto errPort = co_await _PortResolver->Start();
  if (errPort) {
    co_return errPort;
  }

  auto addr = _IpResolver->GetAddress();
  auto port = _PortResolver->GetPort();
  _Endpoint = boost::asio::ip::udp::endpoint(addr, port);
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> ResolverCombinedEndpoint::DoGracefulStop() {
  co_await _IpResolver->Stop();
  co_await _PortResolver->Stop();
  auto& current = co_await Omni::Fiber::GetCurrentFiber();
  co_await current.WaitAll();
  co_return ErrorCode{};
}

} // namespace gh
