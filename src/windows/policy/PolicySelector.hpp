#pragma once

#include <functional>
#include <memory>

#include "ConnectionTracker.hpp"
#include "EndpointWinDivert.hpp"
#include "FlowTracker.hpp"
#include "ProcessTreeTracker.hpp"

namespace gh::policy {

struct WinDivertDeferredPacket : public VpnClientMultiChannel::Mark::Deferred::DeferredPacket {
  WinDivertDeferredPacket(Packet pkt, const WINDIVERT_ADDRESS& addr) : Pkt(std::move(pkt)), Addr(addr) {}
  ~WinDivertDeferredPacket() override = default;

  WinDivertDeferredPacket(const WinDivertDeferredPacket&) = delete;
  auto operator=(const WinDivertDeferredPacket&) -> WinDivertDeferredPacket& = delete;
  WinDivertDeferredPacket(WinDivertDeferredPacket&&) = delete;
  auto operator=(WinDivertDeferredPacket&&) -> WinDivertDeferredPacket& = delete;

  Packet Pkt;
  WINDIVERT_ADDRESS Addr;
};

class PolicySelector : public ConnectionTracker::Selector,
                       public FlowTrackerDeferredCallback,
                       public ProcessTreeTrackerDeferredCallback,
                       public WinDivertRouteCallback {
public:
  explicit PolicySelector(boost::asio::any_io_executor& executor, PolicyRegistry& registry);
  ~PolicySelector() override = default;

  PolicySelector(const PolicySelector&) = delete;
  auto operator=(const PolicySelector&) -> PolicySelector& = delete;
  PolicySelector(PolicySelector&&) = delete;
  auto operator=(PolicySelector&&) -> PolicySelector& = delete;

  void SetConnectionTracker(std::shared_ptr<ConnectionTracker> tracker) { _ConnectionTracker = std::move(tracker); }
  void ClearConnectionTracker() { _ConnectionTracker.reset(); }
  void SetInjector(DeferredPacketInjector& injector) { _Injector = injector; }
  void ClearInjector() { _Injector = std::nullopt; }
  auto GetProcessTreeTracker() -> ProcessTreeTracker& { return *_TreeTracker; }
  auto GetFlowTracker() -> FlowTracker& { return _FlowTracker; }

  auto SelectConnectionMark(const ConnectionTracker::ConnectionKey& key) -> std::shared_ptr<ConnectionMark> override;

  [[nodiscard]] auto ResolvePolicy(const ConnectionTracker::ConnectionKey& key) -> std::shared_ptr<ConnectionMark>;
  auto FlowTrackerContinue(const std::shared_ptr<VpnClientMultiChannel::Mark>& mark, DWORD pid)
      -> Omni::Fiber::Coroutine<void> override;
  auto ProcessTreeTrackerContinue(const std::shared_ptr<VpnClientMultiChannel::Mark>& mark,
                                  const PolicyRule::RoutingAction& action) -> Omni::Fiber::Coroutine<void> override;
  auto WinDivertRoute(Packet& packet, const WINDIVERT_ADDRESS& addr) -> WinDivertRouteCallback::Result override;

private:
  [[nodiscard]] static auto ToConnectionMark(const PolicyRule::RoutingAction& action)
      -> std::shared_ptr<VpnClientMultiChannel::Mark>;

  std::shared_ptr<ConnectionTracker> _ConnectionTracker;
  std::optional<std::reference_wrapper<DeferredPacketInjector>> _Injector;
  FlowTracker _FlowTracker;
  std::shared_ptr<ProcessTreeTracker> _TreeTracker;
};

} // namespace gh::policy
