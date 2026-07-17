#include "PolicySelector.hpp"

#include <memory>
#include <variant>

#include <windows.h>
#include <ws2tcpip.h>

#include <boost/log/trivial.hpp>

#include "Coroutine.hpp"
#include "Utils/Overload.hpp"
#include "VpnClientMultiChannel.hpp"

namespace gh::policy {

PolicySelector::PolicySelector(boost::asio::any_io_executor& executor, PolicyRegistry& registry)
    : _FlowTracker(*this), _TreeTracker(std::make_shared<ProcessTreeTracker>(executor, *this, registry)) {}

auto PolicySelector::SelectConnectionMark(const ConnectionTracker::ConnectionKey& key)
    -> std::shared_ptr<ConnectionMark> {
  return ResolvePolicy(key);
}

auto PolicySelector::ResolvePolicy(const ConnectionTracker::ConnectionKey& key) -> std::shared_ptr<ConnectionMark> {
  auto pid = _FlowTracker.GetPidForConnection(key);
  if (pid.has_value()) {
    auto policy = _TreeTracker->GetPolicy(pid.value());
    if (policy.has_value()) {
      BOOST_LOG_TRIVIAL(trace) << "ResolvePolicy: key=" << key << " pid=" << pid.value()
                               << " resolved to policy=" << policy.value().ToString();
      return ToConnectionMark(policy.value().Action);
    } else {
      BOOST_LOG_TRIVIAL(trace) << "ResolvePolicy: key=" << key << " pid=" << pid.value()
                               << " - no policy found, deferring";
      auto mark = std::make_shared<VpnClientMultiChannel::Mark>(VpnClientMultiChannel::Mark::Deferred{});
      _TreeTracker->AddPendingMark(pid.value(), mark);
      return mark;
    }
  } else {
    BOOST_LOG_TRIVIAL(trace) << "ResolvePolicy: key=" << key << " - no PID found, deferring";
    auto mark = std::make_shared<VpnClientMultiChannel::Mark>(VpnClientMultiChannel::Mark::Deferred{});
    _FlowTracker.AddPendingMark(key, mark);
    return mark;
  }
}

auto PolicySelector::FlowTrackerContinue(const std::shared_ptr<VpnClientMultiChannel::Mark>& mark, DWORD pid)
    -> Omni::Fiber::Coroutine<void> {
  assert(std::holds_alternative<VpnClientMultiChannel::Mark::Deferred>(mark->GetValue()));
  auto policy = _TreeTracker->GetPolicy(pid);
  if (policy.has_value()) {
    BOOST_LOG_TRIVIAL(trace) << "FlowTrackerContinue: pid=" << pid
                             << " - policy found, continuing to ProcessTreeTrackerContinue";
    co_await ProcessTreeTrackerContinue(mark, policy.value().Action);
  } else {
    BOOST_LOG_TRIVIAL(trace) << "FlowTrackerContinue: pid=" << pid << " - no policy found, adding pending mark";
    _TreeTracker->AddPendingMark(pid, mark);
  }
  co_return;
}

auto PolicySelector::ProcessTreeTrackerContinue(const std::shared_ptr<VpnClientMultiChannel::Mark>& mark,
                                                const PolicyRule::RoutingAction& action)
    -> Omni::Fiber::Coroutine<void> {
  assert(std::holds_alternative<VpnClientMultiChannel::Mark::Deferred>(mark->GetValue()));
  auto newMark = ToConnectionMark(action);
  mark->Swap(*newMark);
  auto& packets = std::get<VpnClientMultiChannel::Mark::Deferred>(newMark->GetValue()).Packets;
  BOOST_LOG_TRIVIAL(trace) << "ProcessTreeTrackerContinue: action=" << PolicyRule{action}.ToString()
                           << " injecting " << packets.size() << " packets";
  for (auto& packet : packets) {
    packet.SetMark(mark);
    if (_Injector) {
      co_await _Injector->get().Inject(std::move(packet));
    }
  }
  co_return;
}

auto PolicySelector::ToConnectionMark(const PolicyRule::RoutingAction& action)
    -> std::shared_ptr<VpnClientMultiChannel::Mark> {
  return std::visit(
      Overload{[](const PolicyRule::ByPassRoute&) -> std::shared_ptr<VpnClientMultiChannel::Mark> {
                 return std::make_unique<VpnClientMultiChannel::Mark>(VpnClientMultiChannel::Mark::Bypass{});
               },
               [](const PolicyRule::EndpointRoute& route) -> std::shared_ptr<VpnClientMultiChannel::Mark> {
                 if (auto session = route.Endpoint.lock()) {
                   return std::make_unique<VpnClientMultiChannel::Mark>(session);
                 } else {
                   return std::make_unique<VpnClientMultiChannel::Mark>(VpnClientMultiChannel::Mark::Discard{});
                 }
               }},
      action);
}

} // namespace gh::policy
