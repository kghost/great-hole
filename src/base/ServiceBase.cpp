#include "ServiceBase.hpp"

#include <format>

#include "GetCurrentFiber.hpp"

namespace gh {

Omni::Fiber::Coroutine<ErrorCode> ServiceBase::Start() {
  assert(_State == State::kNone);
  _State = State::kPreStart;
  _Service.emplace();
  Omni::Fiber::Event<ErrorCode> errStart;
  auto& fiber = co_await Omni::Fiber::GetCurrentFiber();
  _Service.value()._Fiber = fiber.Spawn(std::format("Service:{:s}@{:p}", GetName(), static_cast<const void*>(this)),
                                        [this, &errStart]() -> Omni::Fiber::Coroutine<void> {
                                          auto me = shared_from_this(); // Hold me to prevent this from releasing.
                                          assert(me && me.get() == this && "failed to dynamic cast shared_from_this");
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

Omni::Fiber::Coroutine<void> ServiceBase::DoWork() { co_return co_await _Service.value()._Stop.GetFiberCancelEvent(); }

Omni::Fiber::Coroutine<ErrorCode> ServiceBase::Stop() {
  _Service.value()._Stop.Trigger();
  co_return co_await _Service.value()._StopError;
}

Omni::Fiber::Coroutine<void> ServiceBase::WaitService() {
  assert(_State != State::kNone);
  co_await (co_await Omni::Fiber::GetCurrentFiber()).Join(_Service.value()._Fiber);
  _Service.reset();
  _State = State::kNone;
  co_return;
}

} // namespace gh
