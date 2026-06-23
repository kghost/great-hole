#include "ResolverServicePort.hpp"

#include <cstdint>
#if __has_include(<netdb.h>)
#include <netdb.h>
#endif

#include <boost/asio.hpp>

#include "Utils/Endian.hpp"

namespace gh {

ResolverServicePort::ResolverServicePort(std::string const& portStr) : _PortStr(portStr) {}

std::string ResolverServicePort::GetName() const { return std::format("ResolverServicePort:[{}]", _PortStr); }

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
  _Port = ArchEndian(static_cast<decltype(_Port)>(s->s_port));
  _ResolveError = ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> ResolverServicePort::DoGracefulStop() { co_return ErrorCode{}; }

} // namespace gh
