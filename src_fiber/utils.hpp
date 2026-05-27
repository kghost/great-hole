#pragma once

#include <utility>

#include "Coroutine.hpp"
#include "Event.hpp"
#include "GetCurrentFiber.hpp"

namespace gh {

template <typename ResultType, typename StartFunction, typename StopFunction>
  requires(std::invocable<StartFunction> &&
           std::is_same_v<std::invoke_result_t<StartFunction>, Omni::Fiber::Coroutine<ResultType>>)
Omni::Fiber::Coroutine<ResultType> BackgroundStart(bool& is_invoked, Omni::Fiber::Event<ResultType>& started,
                                                   Omni::Fiber::Event<>& stopping, StartFunction&& start,
                                                   StopFunction&& stop) {
  if (std::exchange(is_invoked, true)) {
    co_return co_await started;
  } else {
    auto& fiber = co_await Omni::Fiber::GetCurrentFiber();
    fiber.Spawn(
        "starts",
        [&started, &stopping, start = std::move(start), stop = std::move(stop)]() -> Omni::Fiber::Coroutine<void> {
          started.Fire(co_await start());
          co_await stopping;
          co_await stop();
        });
  }
  co_return co_await started;
}

} // namespace gh
