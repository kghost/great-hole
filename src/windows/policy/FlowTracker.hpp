#pragma once

#include <boost/asio.hpp>
#include <map>
#include <memory>
#include <optional>
#include <vector>
#include <windows.h>

#include "ConnectionTracker.hpp"
#include "VpnClientMultiChannel.hpp"
#include "WinDivertFlowSniffer.hpp"

namespace gh::policy {

class FlowTrackerDeferredCallback {
public:
  explicit FlowTrackerDeferredCallback() = default;
  virtual ~FlowTrackerDeferredCallback() = default;

  FlowTrackerDeferredCallback(const FlowTrackerDeferredCallback&) = delete;
  auto operator=(const FlowTrackerDeferredCallback&) -> FlowTrackerDeferredCallback& = delete;
  FlowTrackerDeferredCallback(FlowTrackerDeferredCallback&&) = delete;
  auto operator=(FlowTrackerDeferredCallback&&) -> FlowTrackerDeferredCallback& = delete;

  virtual auto FlowTrackerContinue(const std::shared_ptr<VpnClientMultiChannel::Mark>& mark, DWORD pid)
      -> Omni::Fiber::Coroutine<void> = 0;
};

class FlowTracker : public WinDivertFlowSnifferCallback {
public:
  explicit FlowTracker(FlowTrackerDeferredCallback& callback);
  ~FlowTracker() override;

  FlowTracker(const FlowTracker&) = delete;
  auto operator=(const FlowTracker&) -> FlowTracker& = delete;
  FlowTracker(FlowTracker&&) = delete;
  auto operator=(FlowTracker&&) -> FlowTracker& = delete;

  // WinDivertFlowSnifferCallback overrides
  auto OnFlowEstablished(FlowKey key, uint32_t pid) -> Omni::Fiber::Coroutine<void> override;
  auto OnFlowDeleted(FlowKey key) -> Omni::Fiber::Coroutine<void> override;

  [[nodiscard]] auto GetPidForConnection(const ConnectionTracker::ConnectionKey& key) -> std::optional<DWORD>;
  void AddPendingMark(const ConnectionTracker::ConnectionKey& key,
                      const std::shared_ptr<VpnClientMultiChannel::Mark>& mark);

  [[nodiscard]] auto GetFlows() const -> std::vector<Interface::FlowInfo>;
  [[nodiscard]] auto GetPendingFlows() const -> std::vector<Interface::PendingFlowInfo>;

  [[nodiscard]] static auto ToFlowKey(const ConnectionTracker::ConnectionKey& key) -> std::optional<FlowKey>;

private:
  FlowTrackerDeferredCallback& _Callback;

  std::map<FlowKey, DWORD> _FlowToPid;
  // TODO: change to weak_ptr
  std::map<FlowKey,
           std::vector<std::pair<ConnectionTracker::ConnectionKey, std::shared_ptr<VpnClientMultiChannel::Mark>>>>
      _PendingFlowResumers;
};

} // namespace gh::policy
