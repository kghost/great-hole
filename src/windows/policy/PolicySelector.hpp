#pragma once

#include <memory>

#include "ConnectionTracker.hpp"
#include "FlowTracker.hpp"
#include "InterfaceWin32.hpp"
#include "Logger.hpp"
#include "ProcessTreeTracker.hpp"
#include "TunnelDataPlane.hpp"

namespace gh::policy {

class PolicySelector : public TunnelDataPlanePolicyResolverCallback {
public:
  explicit PolicySelector(boost::asio::any_io_executor& executor, PolicyRegistry& registry);
  ~PolicySelector() override = default;

  PolicySelector(const PolicySelector&) = delete;
  auto operator=(const PolicySelector&) -> PolicySelector& = delete;
  PolicySelector(PolicySelector&&) = delete;
  auto operator=(PolicySelector&&) -> PolicySelector& = delete;

  auto GetProcessTreeTracker() -> ProcessTreeTracker& { return *_TreeTracker; }
  auto GetFlowTracker() -> FlowTracker& { return _FlowTracker; }

  [[nodiscard]] auto ResolvePolicy(const ConnectionTracker::ConnectionKey& key)
      -> Interface::PolicyRule::RoutingAction override;

private:
  FlowTracker _FlowTracker;
  std::shared_ptr<ProcessTreeTracker> _TreeTracker;
  gh::base::ComponentLogger _Logger{boost::log::keywords::channel = "PolicySelector"};
};

[[nodiscard]] auto ToFlowConnection(const ConnectionTracker::ConnectionKey& key) -> Interface::FlowConnection;

} // namespace gh::policy
