#pragma once

#include <boost/asio.hpp>
#include <map>
#include <optional>
#include <vector>
#include <windows.h>

#include "ConnectionTracker.hpp"
#include "InterfaceWin32.hpp"
#include "WinDivertFlowSniffer.hpp"

namespace gh::policy {

class FlowTracker : public WinDivertFlowSnifferCallback {
public:
  explicit FlowTracker() = default;
  ~FlowTracker() override = default;

  FlowTracker(const FlowTracker&) = delete;
  auto operator=(const FlowTracker&) -> FlowTracker& = delete;
  FlowTracker(FlowTracker&&) = delete;
  auto operator=(FlowTracker&&) -> FlowTracker& = delete;

  // WinDivertFlowSnifferCallback overrides
  auto OnFlowEstablished(const FlowKey& key, Interface::ProcessId process) -> Omni::Fiber::Coroutine<void> override;
  auto OnFlowDeleted(const FlowKey& key) -> Omni::Fiber::Coroutine<void> override;

  [[nodiscard]] auto GetProcessForConnection(const ConnectionTracker::ConnectionKey& key)
      -> std::optional<Interface::ProcessId>;

  [[nodiscard]] auto GetFlows() const -> std::vector<Interface::FlowInfo>;

  [[nodiscard]] static auto ToFlowWildcardKey(const ConnectionTracker::ConnectionKey& key) -> std::optional<FlowKey>;
  [[nodiscard]] static auto ToFlowExactKey(const ConnectionTracker::ConnectionKey& key) -> std::optional<FlowKey>;

private:
  std::map<FlowKey, Interface::ProcessId> _FlowToProcess;
  gh::base::ComponentLogger _Logger{boost::log::keywords::channel = "FlowTracker"};
};

} // namespace gh::policy
