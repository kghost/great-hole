#include "PolicyRegistry.hpp"

#include "Utils/Overload.hpp"
#include "VpnClientMultiChannel.hpp"

namespace gh::policy {

void PolicyRegistry::Clear() {
  _PathRules.clear();
  _DefaultRoute = PolicyRule::ByPassRoute{};
}

void PolicyRegistry::AddPathRule(const std::string& pathPattern, const PolicyRule& rule) {
  _PathRules[pathPattern] = rule;
}

void PolicyRegistry::RemovePathRule(const std::string& pathPattern) {
  _PathRules.erase(pathPattern);
}

auto PolicyRegistry::GetRuleForPath(const std::string& path) const -> std::optional<PolicyRule> {
  // Try exact match first
  auto iterator = _PathRules.find(path);
  if (iterator != _PathRules.end()) {
    return iterator->second;
  }

  return std::nullopt;
}

void PolicyRegistry::SetDefaultAction(const PolicyRule::RoutingAction& action) {
  _DefaultRoute = action;
}

auto PolicyRegistry::GetDefaultAction() const -> PolicyRule::RoutingAction {
  return _DefaultRoute;
}

} // namespace gh::policy

namespace gh::Interface {

auto PolicyRule::ToString() const -> std::string {
  return std::visit(
      Overload{
          [](ByPassRoute) -> std::string { return "ByPass"; },
          [](const EndpointRoute& route) -> std::string {
            if (auto session = route.Endpoint.lock()) {
              return std::format("Endpoint[{}]", session->GetDescription());
            } else {
              return "Endpoint[Invalid]";
            }
          },
      },
      Action);
}

} // namespace gh::Interface
