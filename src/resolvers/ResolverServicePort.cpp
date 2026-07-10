#include "ResolverServicePort.hpp"

#include <cstdint>
#if __has_include(<netdb.h>)
#include <netdb.h>
#endif

#include <boost/asio.hpp>

#include "Utils/Endian.hpp"

namespace gh {

ResolverServicePort::ResolverServicePort(std::string const& portStr) : _PortStr(portStr) {}

auto ResolverServicePort::GetName() const -> std::string { return std::format("ResolverServicePort:[{}]", _PortStr); }

auto ResolverServicePort::GetResolverResult() const -> uint16_t { return _Port; }

auto ResolverServicePort::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> { co_return ErrorCode{}; }

auto ResolverServicePort::DoWork() -> Omni::Fiber::Coroutine<void> {
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

auto ResolverServicePort::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> { co_return ErrorCode{}; }

} // namespace gh
