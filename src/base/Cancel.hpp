#pragma once

#include <boost/asio.hpp>
#include <boost/log/trivial.hpp>
#include <functional>
#include <set>

#ifdef _WIN32
#include <windows.h>
#endif

#include "Asio.hpp"
#include "Event.hpp"
#include "Utils.hpp"

namespace gh {

class Cancel {
public:
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

  class Tracker {
  public:
    explicit Tracker() = default;
    virtual ~Tracker() = default;

    Tracker(const Tracker&) = default;
    auto operator=(const Tracker&) -> Tracker& = default;
    Tracker(Tracker&&) = delete;
    auto operator=(Tracker&&) -> Tracker& = delete;

    virtual void Emit() = 0;
  };

  class SlotTracker : public Tracker {
  public:
    explicit SlotTracker(Cancel& parent) : _Parent(parent) { _Parent.Register(*this); }
    ~SlotTracker() override { _Parent.Unregister(*this); }

    SlotTracker(const SlotTracker&) = delete;
    auto operator=(const SlotTracker&) -> SlotTracker& = delete;
    SlotTracker(SlotTracker&&) = delete;
    auto operator=(SlotTracker&&) -> SlotTracker& = delete;

    auto operator()() -> decltype(auto) {
      return boost::asio::bind_cancellation_slot(_Signal.slot(), Omni::Fiber::AsioUseFiber);
    }

    void Emit() override { _Signal.emit(boost::asio::cancellation_type::total); }

  private:
    Cancel& _Parent;
    boost::asio::cancellation_signal _Signal;
  };

#ifdef _WIN32
  class HandleTracker : public Tracker {
  public:
    explicit HandleTracker(Cancel& parent, HANDLE handle, LPOVERLAPPED overlapped = nullptr)
        : _Parent(parent), _Handle(handle), _Overlapped(overlapped) {
      _Parent.Register(*this);
    }
    ~HandleTracker() override { _Parent.Unregister(*this); }

    HandleTracker(const HandleTracker&) = delete;
    auto operator=(const HandleTracker&) -> HandleTracker& = delete;
    HandleTracker(HandleTracker&&) = delete;
    auto operator=(HandleTracker&&) -> HandleTracker& = delete;

    void Emit() override {
      if (_Handle != INVALID_HANDLE_VALUE && _Handle != nullptr) {
        ::CancelIoEx(_Handle, _Overlapped);
      }
    }

  private:
    Cancel& _Parent;
    HANDLE _Handle;
    LPOVERLAPPED _Overlapped;
  };
#endif

  auto AsioSlot() -> SlotTracker { return SlotTracker(*this); }

private:
  void Register(Tracker& tracker) {
    _Trackers.insert(tracker);
    if (IsTriggered()) {
      tracker.Emit();
    }
  }

  void Unregister(Tracker& tracker) { _Trackers.erase(tracker); }

  Omni::Fiber::Event<void> _CancelEvent;
  std::set<std::reference_wrapper<Tracker>, Less<Tracker>> _Trackers;
};

} // namespace gh
