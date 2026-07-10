#include "ServiceBase.hpp"

#include <boost/log/trivial.hpp>
#include <cassert>
#include <format>

#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "Event.hpp"
#include "GetCurrentOmniFiber.hpp"

namespace gh {

auto ServiceBase::Start() -> Omni::Fiber::Coroutine<ErrorCode> {
  assert(_State == State::kNone);
  _State = State::kPreStart;
  _Service.emplace();
  Omni::Fiber::Event<ErrorCode> errStart;
  auto& fiber = co_await Omni::Fiber::GetCurrentOmniFiber();
  _Service.value()._Fiber =
      fiber.Spawn(std::format("Service:{:s}@{:p}", GetName(), static_cast<const void*>(this)),
                  [this, &errStart]() -> Omni::Fiber::Coroutine<void> {
                    auto self = shared_from_this(); // Hold me to prevent this from releasing.
                    assert(self && self.get() == this && "failed to dynamic cast shared_from_this");
                    assert(_State == State::kPreStart);
                    _State = State::kStarting;
                    BOOST_LOG_TRIVIAL(info) << GetName() << " starting";
                    auto err = co_await DoStart();
                    errStart.Fire(err);
                    if (err) {
                      _State = State::kError;
                      _Service.value()._StopError.Fire(err);
                      BOOST_LOG_TRIVIAL(info) << GetName() << " starts failed: " << err;
                      co_return;
                    }

                    _State = State::kRunning;
                    BOOST_LOG_TRIVIAL(info) << GetName() << " started";
                    co_await DoWork();

                    _State = State::kStopping;
                    BOOST_LOG_TRIVIAL(info) << GetName() << " stopping";
                    auto errStop = co_await DoGracefulStop();
                    _Service.value()._StopError.Fire(errStop);
                    if (!errStop) {
                      _State = State::kFinished;
                      BOOST_LOG_TRIVIAL(info) << GetName() << " stopped";
                    } else {
                      _State = State::kError;
                      BOOST_LOG_TRIVIAL(info) << GetName() << " stops failed: " << errStop;
                    }
                  });
  co_return co_await errStart;
}

auto ServiceBase::DoWork() -> Omni::Fiber::Coroutine<void> {
  co_return co_await _Service.value()._Stop.GetFiberCancelEvent();
}

auto ServiceBase::Stop() -> Omni::Fiber::Coroutine<ErrorCode> {
  if (!_Service.has_value()) {
    co_return ErrorCode{};
  }
  _Service.value()._Stop.Trigger();
  co_await (co_await Omni::Fiber::GetCurrentOmniFiber()).Join(_Service.value()._Fiber);
  auto err = co_await _Service.value()._StopError;
  _Service.reset();
  _State = State::kNone;
  co_return err;
}

} // namespace gh
