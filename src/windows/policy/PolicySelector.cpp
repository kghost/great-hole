#include "PolicySelector.hpp"

#include <memory>
#include <variant>

#include <windows.h>
#include <ws2tcpip.h>

#include "Coroutine.hpp"
#include "Utils/Overload.hpp"
#include "VpnClientMultiChannel.hpp"

namespace gh::policy {

PolicySelector::PolicySelector(boost::asio::any_io_executor& executor, DeferredPacketInjector& injector,
                               PolicyRegistry& registry)
    : _Injector(injector), _FlowTracker(*this),
      _TreeTracker(std::make_shared<ProcessTreeTracker>(executor, *this, registry)) {}

auto PolicySelector::operator()(const ConnectionTracker::ConnectionKey& key) -> std::shared_ptr<ConnectionMark> {
  return ResolvePolicy(key);
}

auto PolicySelector::ResolvePolicy(const ConnectionTracker::ConnectionKey& key) -> std::shared_ptr<ConnectionMark> {
  auto pid = _FlowTracker.GetPidForConnection(key);
  if (pid.has_value()) {
    auto policy = _TreeTracker->GetPolicy(pid.value());
    if (policy.has_value()) {
      return ToConnectionMark(policy.value().Action);
    } else {
      auto mark = std::make_shared<VpnClientMultiChannel::Mark>(VpnClientMultiChannel::Mark::Deferred{});
      _TreeTracker->AddPendingMark(pid.value(), mark);
      return mark;
    }
  } else {
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
    co_await ProcessTreeTrackerContinue(mark, policy.value().Action);
  } else {
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
  for (auto& packet : packets) {
    packet.SetMark(mark);
    co_await _Injector.Inject(std::move(packet));
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
