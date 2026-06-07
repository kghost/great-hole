#pragma once

#include <boost/asio.hpp>
#include <functional>
#include <set>

#include "Asio.hpp"
#include "Event.hpp"
#include "Utils.hpp"

namespace gh {

class Cancel {
public:
  class SlotTracker;

  explicit Cancel() = default;
  ~Cancel() = default;

  Cancel(const Cancel&) = delete;
  Cancel& operator=(const Cancel&) = delete;
  Cancel(Cancel&&) = delete;
  Cancel& operator=(Cancel&&) = delete;

  bool IsTriggered() const { return _CancelEvent.IsFired(); }

  void Trigger() {
    _CancelEvent.Fire();
    for (auto& tracker : _Trackers) {
      tracker.get().Emit();
    }
  }

  Omni::Fiber::Event<void>& GetFiberCancelEvent() { return _CancelEvent; }

  class SlotTracker {
  public:
    explicit SlotTracker(Cancel& parent) : _Parent(parent) { _Parent.Register(*this); }
    ~SlotTracker() { _Parent.Unregister(*this); }

    SlotTracker(const SlotTracker&) = delete;
    SlotTracker& operator=(const SlotTracker&) = delete;
    SlotTracker(SlotTracker&&) = delete;
    SlotTracker& operator=(SlotTracker&&) = delete;

    boost::asio::cancellation_slot Slot() { return _Signal.slot(); }

    void Emit() { _Signal.emit(boost::asio::cancellation_type::total); }

  private:
    Cancel& _Parent;
    boost::asio::cancellation_signal _Signal;
  };

  SlotTracker AsioSlot() { return SlotTracker(*this); }

  decltype(auto) AsioToken() {
    return boost::asio::bind_cancellation_slot(AsioSlot().Slot(), Omni::Fiber::AsioUseFiber);
  }

private:
  void Register(SlotTracker& tracker) {
    _Trackers.insert(tracker);
    if (IsTriggered()) {
      tracker.Emit();
    }
  }

  void Unregister(SlotTracker& tracker) { _Trackers.erase(tracker); }

  Omni::Fiber::Event<void> _CancelEvent;
  std::set<std::reference_wrapper<SlotTracker>, Less<SlotTracker>> _Trackers;
};

} // namespace gh
