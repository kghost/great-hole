#include "ResolverServicePort.hpp"

#include <arpa/inet.h>
#include <netdb.h>

#include <boost/asio.hpp>

namespace gh {

ResolverServicePort::ResolverServicePort(std::string const& portStr) : _PortStr(portStr) {}

std::string ResolverServicePort::GetName() const { return "ResolverServicePort:" + _PortStr; }

uint16_t ResolverServicePort::GetResolverResult() const { return _Port; }

Omni::Fiber::Coroutine<ErrorCode> ResolverServicePort::DoStart() { co_return ErrorCode{}; }

Omni::Fiber::Coroutine<void> ResolverServicePort::DoWork() {
  if (_Service.value()._Stop.IsTriggered()) {
    _ResolveError = make_error_code(boost::asio::error::operation_aborted);
    co_return;
  }
  struct servent* s = getservbyname(_PortStr.c_str(), nullptr);
  if (s == nullptr) {
    _ResolveError = make_error_code(boost::asio::error::invalid_argument);
    co_return;
  }
  _Port = ntohs(s->s_port);
  _ResolveError = ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> ResolverServicePort::DoGracefulStop() { co_return ErrorCode{}; }

} // namespace gh
