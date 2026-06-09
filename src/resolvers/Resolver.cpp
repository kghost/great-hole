#include "Resolver.hpp"

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "ErrorCode.hpp"

namespace gh {

Omni::Fiber::Coroutine<ErrorCode> ResolverBase::DoResolve() {
  _ResolveError = ErrorCode{};
  auto errStart = co_await Start();
  if (errStart) {
    co_await WaitService();
    co_return errStart;
  }

  co_await _Service.value()._StopError;
  co_await WaitService();
  co_return _ResolveError;
}

} // namespace gh
