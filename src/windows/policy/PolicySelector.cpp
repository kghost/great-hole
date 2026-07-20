#include "PolicySelector.hpp"

#include <memory>
#include <variant>

#include <windows.h>
#include <ws2tcpip.h>

#include <boost/log/trivial.hpp>

#include "Coroutine.hpp"
#include "PolicyRegistry.hpp"
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
    auto action = _TreeTracker->GetAction(pid.value());
    if (action.has_value()) {
      BOOST_LOG_TRIVIAL(trace) << "ResolvePolicy: key=" << key << " pid=" << pid.value()
                               << " resolved to action=" << PolicyActionToString(action.value());
      return ToConnectionMark(action.value());
    } else {
      BOOST_LOG_TRIVIAL(trace) << "ResolvePolicy: key=" << key << " pid=" << pid.value()
                               << " - no action found, deferring";
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
  auto action = _TreeTracker->GetAction(pid);
  if (action.has_value()) {
    BOOST_LOG_TRIVIAL(trace) << "FlowTrackerContinue: pid=" << pid
                             << " - action found, continuing to ProcessTreeTrackerContinue";
    co_await ProcessTreeTrackerContinue(mark, action.value());
  } else {
    BOOST_LOG_TRIVIAL(trace) << "FlowTrackerContinue: pid=" << pid << " - no action found, adding pending mark";
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
  BOOST_LOG_TRIVIAL(trace) << "ProcessTreeTrackerContinue: action=" << PolicyActionToString(action) << " injecting "
                           << packets.size() << " packets";
  for (auto& deferredPacket : packets) {
    auto* winDivertPacket = dynamic_cast<WinDivertDeferredPacket*>(deferredPacket.get());
    if (winDivertPacket == nullptr) {
      continue;
    }
    winDivertPacket->Pkt.SetMark(mark);
    auto route =
        std::visit(Overload{
                       [](VpnClientMultiChannel::Mark::ToBeSelected) -> WinDivertRouteCallback::Result {
                         assert(false && "should not reach here");
                         std::unreachable();
                       },
                       [](VpnClientMultiChannel::Mark::Bypass) -> WinDivertRouteCallback::Result {
                         return WinDivertRouteCallback::Result::Bypass;
                       },
                       [](VpnClientMultiChannel::Mark::Discard) -> WinDivertRouteCallback::Result {
                         return WinDivertRouteCallback::Result::Discard;
                       },
                       [](const VpnClientMultiChannel::Mark::Deferred&) -> WinDivertRouteCallback::Result {
                         assert(false && "should not reach here");
                         std::unreachable();
                       },
                       [](const std::weak_ptr<VpnClientMultiChannelSession>&) -> WinDivertRouteCallback::Result {
                         return WinDivertRouteCallback::Result::Normal;
                       },
                   },
                   mark->GetValue());

    if (_Injector) {
      co_await _Injector->get().Inject(std::move(winDivertPacket->Pkt), winDivertPacket->Addr, route);
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

auto PolicySelector::WinDivertRoute(Packet& packet, const WINDIVERT_ADDRESS& addr) -> WinDivertRouteCallback::Result {
  if (addr.Loopback || !addr.Outbound) {
    return WinDivertRouteCallback::Result::Normal;
  }
  assert(_ConnectionTracker);
  auto result = _ConnectionTracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(packet, *this);
  if (result.has_value()) {
    auto mark = std::dynamic_pointer_cast<VpnClientMultiChannel::Mark>(result.value());
    packet.SetMark(mark);

    return std::visit(
        Overload{
            [](VpnClientMultiChannel::Mark::ToBeSelected) -> gh::WinDivertRouteCallback::Result {
              assert(false && "should not reach here");
              std::unreachable();
            },
            [](VpnClientMultiChannel::Mark::Bypass) -> gh::WinDivertRouteCallback::Result {
              return WinDivertRouteCallback::Result::Bypass;
            },
            [](VpnClientMultiChannel::Mark::Discard) -> gh::WinDivertRouteCallback::Result {
              return WinDivertRouteCallback::Result::Discard;
            },
            [&packet, &addr](VpnClientMultiChannel::Mark::Deferred& deferred) -> gh::WinDivertRouteCallback::Result {
              deferred.Packets.push_back(std::make_unique<WinDivertDeferredPacket>(std::move(packet), addr));
              return WinDivertRouteCallback::Result::Discard;
            },
            [](const std::weak_ptr<VpnClientMultiChannelSession>&) -> gh::WinDivertRouteCallback::Result {
              return WinDivertRouteCallback::Result::Normal;
            },
        },
        mark->GetValue());
  } else {
    BOOST_LOG_TRIVIAL(warning) << "WinDivert: LookupAndUpdate bypass failed: " << result.error().message();
    return WinDivertRouteCallback::Result::Normal;
  }
}

} // namespace gh::policy
