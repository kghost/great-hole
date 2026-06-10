#include "Resolver.hpp"

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"

namespace gh {

Omni::Fiber::Coroutine<ErrorCode> ResolverBase::DoResolve(Cancel& c) {
  _ResolveError = ErrorCode{};
  auto errStart = co_await Start();
  if (errStart) {
    co_await WaitService();
    co_return errStart;
  }

  auto [cancelled, stopped] =
      co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(c.GetFiberCancelEvent(), [] {}),
                                   Omni::Fiber::SelectPair(_Service.value()._StopError, [](auto) {}));

  if (cancelled) {
    co_await Stop();
  }

  co_await WaitService();

  if (_ResolveError) {
    co_return _ResolveError;
  }
  if (cancelled) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }

  co_return ErrorCode{};
}

} // namespace gh
