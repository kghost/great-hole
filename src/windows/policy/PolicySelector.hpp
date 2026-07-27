#pragma once

#include <memory>

#include "ConnectionTracker.hpp"
#include "EndpointWinDivert.hpp"
#include "FlowTracker.hpp"
#include "Logger.hpp"
#include "ProcessTreeTracker.hpp"
#include "VpnClientMultiChannel.hpp"

namespace gh::policy {

class PolicySelector : public ConnectionTracker::Selector, public WinDivertRouteCallback {
public:
  explicit PolicySelector(boost::asio::any_io_executor& executor, PolicyRegistry& registry);
  ~PolicySelector() override = default;

  PolicySelector(const PolicySelector&) = delete;
  auto operator=(const PolicySelector&) -> PolicySelector& = delete;
  PolicySelector(PolicySelector&&) = delete;
  auto operator=(PolicySelector&&) -> PolicySelector& = delete;

  void SetConnectionTracker(std::shared_ptr<ConnectionTracker> tracker) { _ConnectionTracker = std::move(tracker); }
  void ClearConnectionTracker() { _ConnectionTracker.reset(); }
  auto GetProcessTreeTracker() -> ProcessTreeTracker& { return *_TreeTracker; }
  auto GetFlowTracker() -> FlowTracker& { return _FlowTracker; }
  [[nodiscard]] auto GetConnections() const -> std::vector<Interface::TrackedConnectionInfo>;

  auto SelectConnectionMark(const ConnectionTracker::ConnectionKey& key) -> std::shared_ptr<ConnectionMark> override;

  [[nodiscard]] auto ResolvePolicy(const ConnectionTracker::ConnectionKey& key) -> std::shared_ptr<ConnectionMark>;
  auto WinDivertRoute(Packet& packet, const WINDIVERT_ADDRESS& addr) -> WinDivertRouteCallback::Result override;

private:
  [[nodiscard]] static auto ToConnectionMark(const PolicyRule::RoutingAction& action)
      -> std::shared_ptr<VpnClientMultiChannel::Mark>;

  std::shared_ptr<ConnectionTracker> _ConnectionTracker;
  FlowTracker _FlowTracker;
  std::shared_ptr<ProcessTreeTracker> _TreeTracker;
  gh::base::ComponentLogger _Logger{boost::log::keywords::channel = "PolicySelector"};
};

[[nodiscard]] auto ToFlowConnection(const ConnectionTracker::ConnectionKey& key) -> Interface::FlowConnection;

} // namespace gh::policy
