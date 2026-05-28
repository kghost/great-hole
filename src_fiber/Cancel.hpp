#pragma once

#include <boost/asio.hpp>
#include <lua.hpp>

#include "Event.hpp"

namespace gh {

class Cancel {
public:
  explicit Cancel() {}
  ~Cancel() = default;

  Cancel(const Cancel&) = delete;
  Cancel& operator=(const Cancel&) = delete;
  Cancel(Cancel&&) = delete;
  Cancel& operator=(Cancel&&) = delete;

  bool IsTriggered() const { return _CancelEvent.IsFired(); }

  void Trigger() {
    _AsioSignal.emit(boost::asio::cancellation_type::total);
    _CancelEvent.Fire();
  }

  boost::asio::cancellation_slot GetAsioCancelSlot() { return _AsioSignal.slot(); }
  Omni::Fiber::Event<>& GetFiberCancelEvent() { return _CancelEvent; }

private:
  boost::asio::cancellation_signal _AsioSignal;
  Omni::Fiber::Event<> _CancelEvent;
};

} // namespace gh
