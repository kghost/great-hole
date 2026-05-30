#include "ResolverNumberPort.hpp"

#include <boost/asio.hpp>
#include <boost/lexical_cast.hpp>

namespace gh {

ResolverNumberPort::ResolverNumberPort(uint16_t port) : _Port(port) {}

std::string ResolverNumberPort::GetName() const {
  return "ResolverNumberPort:" + boost::lexical_cast<std::string>(_Port);
}

uint16_t ResolverNumberPort::GetResolverResult() const { return _Port; }

Omni::Fiber::Coroutine<ErrorCode> ResolverNumberPort::DoStart() { co_return ErrorCode{}; }

Omni::Fiber::Coroutine<ErrorCode> ResolverNumberPort::DoGracefulStop() { co_return ErrorCode{}; }

} // namespace gh
