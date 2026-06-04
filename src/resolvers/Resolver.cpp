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

  // TODO: resolvers should do their work in DoWork. not in Start, move work from Start to DoWork, and only do state
  // transition in Start.

  auto errStop = co_await Stop();
  co_await WaitService();
  if (errStop) {
    co_return errStop;
  }
  co_return ErrorCode{};
}

} // namespace gh
