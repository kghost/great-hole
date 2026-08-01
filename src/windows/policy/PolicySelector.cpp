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
  auto process = _FlowTracker.GetProcessForConnection(key);
  if (process.has_value()) {
    auto action = _TreeTracker->GetAction(process.value());
    if (action.has_value()) {
      BOOST_LOG_SEV(_Logger, boost::log::trivial::trace)
          << "ResolvePolicy: key=" << key << " process=" << process.value()
          << " resolved to action=" << PolicyActionToString(action.value());
      return action.value();
    } else {
      BOOST_LOG_SEV(_Logger, boost::log::trivial::trace)
          << "ResolvePolicy: key=" << key << " process=" << process.value() << " - no action found, defaulting to "
          << PolicyActionToString(Interface::PolicyRule::ByPassRoute{});
      return Interface::PolicyRule::ByPassRoute{};
    }
  } else {
    BOOST_LOG_SEV(_Logger, boost::log::trivial::trace)
        << "ResolvePolicy: key=" << key << " - no process found, defaulting to "
        << PolicyActionToString(Interface::PolicyRule::ByPassRoute{});
    return Interface::PolicyRule::ByPassRoute{};
  }
}

} // namespace gh::policy
