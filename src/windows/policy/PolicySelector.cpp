#include "PolicySelector.hpp"

#include <memory>

#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/trivial.hpp>

#include <windows.h>
#include <ws2tcpip.h>

#include "PolicyRegistry.hpp"

namespace gh::policy {

PolicySelector::PolicySelector(boost::asio::any_io_executor& executor, PolicyRegistry& registry)
    : _TreeTracker(std::make_shared<ProcessTreeTracker>(executor, registry)) {}

auto PolicySelector::ResolvePolicy(const ConnectionTracker::ConnectionKey& key)
    -> Interface::PolicyRule::RoutingAction {
  auto pid = _FlowTracker.GetPidForConnection(key);
  if (pid.has_value()) {
    auto action = _TreeTracker->GetAction(pid.value());
    if (action.has_value()) {
      BOOST_LOG_SEV(_Logger, boost::log::trivial::trace)
          << "ResolvePolicy: key=" << key << " pid=" << pid.value()
          << " resolved to action=" << PolicyActionToString(action.value());
      return action.value();
    }
  }

  BOOST_LOG_SEV(_Logger, boost::log::trivial::trace)
      << "ResolvePolicy: key=" << key << " - no PID/action found, defaulting to bypass mark";
  return Interface::PolicyRule::ByPassRoute{};
}

} // namespace gh::policy
