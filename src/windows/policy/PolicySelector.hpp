#pragma once

#include <memory>

#include "ConnectionTracker.hpp"
#include "DeferredPacketInjector.hpp"
#include "FlowTracker.hpp"
#include "ProcessTreeTracker.hpp"

namespace gh::policy {

class PolicySelector : public ConnectionTracker::Selector,
                       public FlowTrackerDeferredCallback,
                       public ProcessTreeTrackerDeferredCallback {
public:
  explicit PolicySelector(boost::asio::any_io_executor& executor, DeferredPacketInjector& injector,
                          PolicyRegistry& registry);
  ~PolicySelector() override = default;

  PolicySelector(const PolicySelector&) = delete;
  auto operator=(const PolicySelector&) -> PolicySelector& = delete;
  PolicySelector(PolicySelector&&) = delete;
  auto operator=(PolicySelector&&) -> PolicySelector& = delete;

  auto GetProcessTreeTracker() -> ProcessTreeTracker& { return *_TreeTracker; }
  auto GetFlowTracker() -> FlowTracker& { return _FlowTracker; }

  auto operator()(const ConnectionTracker::ConnectionKey& key) -> std::shared_ptr<ConnectionMark> override;

  [[nodiscard]] auto ResolvePolicy(const ConnectionTracker::ConnectionKey& key) -> std::shared_ptr<ConnectionMark>;
  auto FlowTrackerContinue(const std::shared_ptr<VpnClientMultiChannel::Mark>& mark, DWORD pid)
      -> Omni::Fiber::Coroutine<void> override;
  auto ProcessTreeTrackerContinue(const std::shared_ptr<VpnClientMultiChannel::Mark>& mark,
                                  const PolicyRule::RoutingAction& action) -> Omni::Fiber::Coroutine<void> override;

private:
  [[nodiscard]] static auto ToConnectionMark(const PolicyRule::RoutingAction& action)
      -> std::shared_ptr<VpnClientMultiChannel::Mark>;

  DeferredPacketInjector& _Injector;
  FlowTracker _FlowTracker;
  std::shared_ptr<ProcessTreeTracker> _TreeTracker;
};

} // namespace gh::policy
