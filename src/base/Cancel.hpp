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
  auto operator=(const Cancel&) -> Cancel& = delete;
  Cancel(Cancel&&) = delete;
  auto operator=(Cancel&&) -> Cancel& = delete;

  [[nodiscard]] auto IsTriggered() const -> bool { return _CancelEvent.IsFired(); }

  void Trigger() {
    _CancelEvent.Fire();
    for (const auto& tracker : _Trackers) {
      tracker.get().Emit();
    }
  }

  auto GetFiberCancelEvent() -> Omni::Fiber::Event<void>& { return _CancelEvent; }

  class SlotTracker {
  public:
    explicit SlotTracker(Cancel& parent) : _Parent(parent) { _Parent.Register(*this); }
    ~SlotTracker() { _Parent.Unregister(*this); }

    SlotTracker(const SlotTracker&) = delete;
    auto operator=(const SlotTracker&) -> SlotTracker& = delete;
    SlotTracker(SlotTracker&&) = delete;
    auto operator=(SlotTracker&&) -> SlotTracker& = delete;

    auto operator()() -> decltype(auto) {
      return boost::asio::bind_cancellation_slot(_Signal.slot(), Omni::Fiber::AsioUseFiber);
    }

    void Emit() { _Signal.emit(boost::asio::cancellation_type::total); }

  private:
    Cancel& _Parent;
    boost::asio::cancellation_signal _Signal;
  };

  auto AsioSlot() -> SlotTracker { return SlotTracker(*this); }

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
