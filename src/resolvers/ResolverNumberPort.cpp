#include "ResolverNumberPort.hpp"

#include <boost/asio.hpp>
#include <boost/lexical_cast.hpp>

namespace gh {

ResolverNumberPort::ResolverNumberPort(uint16_t port) : _Port(port) {}

auto ResolverNumberPort::GetName() const -> std::string {
  return "ResolverNumberPort:" + boost::lexical_cast<std::string>(_Port);
}

auto ResolverNumberPort::GetResolverResult() const -> uint16_t { return _Port; }

auto ResolverNumberPort::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> { co_return ErrorCode{}; }
auto ResolverNumberPort::DoWork() -> Omni::Fiber::Coroutine<void> {
  _ResolveError = ErrorCode{};
  co_return;
}
auto ResolverNumberPort::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> { co_return ErrorCode{}; }

} // namespace gh
