#include "Resolver.hpp"

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"

namespace gh {

auto Resolver::DoResolve(Cancel& cancel) -> Omni::Fiber::Coroutine<ErrorCode> {
  _ResolveError = ErrorCode{};
  auto errStart = co_await Start();
  if (errStart) {
    co_await Stop();
    co_return errStart;
  }

  auto [cancelled, stopped] =
      co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(cancel.GetFiberCancelEvent(), [] -> void {}),
                                   Omni::Fiber::SelectPair(_Service.value()._StopError, [](auto) -> auto {}));

  co_await Stop();

  if (_ResolveError) {
    co_return _ResolveError;
  }
  if (cancelled) {
    co_return Error(AppErrorCategory::kOperationAborted);
  }

  co_return ErrorCode{};
}

} // namespace gh
