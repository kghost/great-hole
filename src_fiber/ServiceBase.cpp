#include "ServiceBase.hpp"

#include <format>

#include "GetCurrentFiber.hpp"

namespace gh {

Omni::Fiber::Coroutine<ErrorCode> ServiceBase::Start() {
  Omni::Fiber::Event<ErrorCode> errStart;
  auto& fiber = co_await Omni::Fiber::GetCurrentFiber();
  fiber.Spawn(std::format("Service:{:s}@{:p}", GetName(), static_cast<const void*>(this)),
              [this, &errStart]() -> Omni::Fiber::Coroutine<void> {
                auto me = shared_from_this(); // Hold me to prevent this from releasing.
                assert(me && me.get() == this && "failed to dynamic cast shared_from_this to Udp");
                BOOST_LOG_TRIVIAL(info) << GetName() << " starting";
                errStart.Fire(co_await DoStart());
                BOOST_LOG_TRIVIAL(info) << GetName() << " started";
                BOOST_LOG_TRIVIAL(info) << GetName() << " stopping";
                _StopError.Fire(co_await DoGracefulStop());
                BOOST_LOG_TRIVIAL(info) << GetName() << " stopped";
              });
  co_return co_await errStart;
}

Omni::Fiber::Coroutine<void> ServiceBase::DoWork() { co_return co_await _Stop.GetFiberCancelEvent(); }

Omni::Fiber::Coroutine<ErrorCode> ServiceBase::Stop() {
  _Stop.Trigger();
  co_return co_await _StopError;
}

} // namespace gh
