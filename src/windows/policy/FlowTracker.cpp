#include "FlowTracker.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/log/trivial.hpp>
#include <optional>

namespace gh::policy {

FlowTracker::FlowTracker(FlowTrackerDeferredCallback& callback) : _Callback(callback) {}
FlowTracker::~FlowTracker() { _PendingFlowResumers.clear(); }

auto FlowTracker::GetPidForConnection(const ConnectionTracker::ConnectionKey& key) -> std::optional<DWORD> {
  auto iterator = _FlowToPid.find(key);
  if (iterator != _FlowToPid.end()) {
    return iterator->second;
  } else {
    return std::nullopt;
  }
}

void FlowTracker::AddPendingMark(const ConnectionTracker::ConnectionKey& key,
                                 const std::shared_ptr<VpnClientMultiChannel::Mark>& mark) {
  _PendingFlowResumers.try_emplace(key, mark);
}

auto FlowTracker::OnFlowEstablished(const ConnectionTracker::ConnectionKey& conn, uint32_t pid)
    -> Omni::Fiber::Coroutine<void> {
  if (std::holds_alternative<ConnectionTracker::IcmpKey>(conn)) {
    BOOST_LOG_TRIVIAL(info) << "FlowTracker: ICMPv4 flow established: " << conn << ", PID: " << pid;
  } else if (std::holds_alternative<ConnectionTracker::Icmp6Key>(conn)) {
    BOOST_LOG_TRIVIAL(info) << "FlowTracker: ICMPv6 flow established: " << conn << ", PID: " << pid;
  }

  auto [iterator, inserted] = _FlowToPid.try_emplace(conn, pid);
  if (inserted) {
    if (auto resumerIter = _PendingFlowResumers.find(conn); resumerIter != _PendingFlowResumers.end()) {
      co_await _Callback.FlowTrackerContinue(resumerIter->second, pid);
      _PendingFlowResumers.erase(resumerIter);
    }
  }
  co_return;
}

auto FlowTracker::OnFlowDeleted(const ConnectionTracker::ConnectionKey& conn) -> Omni::Fiber::Coroutine<void> {
  _FlowToPid.erase(conn);
  co_return;
}

} // namespace gh::policy
