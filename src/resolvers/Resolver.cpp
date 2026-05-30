#include "Resolver.hpp"

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "ErrorCode.hpp"

namespace gh {

Omni::Fiber::Coroutine<ErrorCode> ResolverBase::DoResolve() {
  auto errStart = co_await Start();
  if (errStart) {
    co_await WaitService();
    co_return errStart;
  }

  auto errStop = co_await Stop();
  co_await WaitService();
  if (errStop) {
    co_return errStop;
  }
  co_return ErrorCode{};
}

} // namespace gh
