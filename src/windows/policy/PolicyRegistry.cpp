#include "PolicyRegistry.hpp"

#include <format>

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

void PolicyRegistry::RemovePathRule(const std::string& pathPattern) { _PathRules.erase(pathPattern); }

auto PolicyRegistry::GetRuleForPath(const std::string& path) const -> std::optional<PolicyRule> {
  // Try exact match first
  auto iterator = _PathRules.find(path);
  if (iterator != _PathRules.end()) {
    return iterator->second;
  }

  return std::nullopt;
}

void PolicyRegistry::SetDefaultAction(const PolicyRule::RoutingAction& action) { _DefaultRoute = action; }

auto PolicyRegistry::GetDefaultAction() const -> PolicyRule::RoutingAction { return _DefaultRoute; }

auto PolicyRegistry::GetCurrentProcessAction() -> PolicyRule::RoutingAction { return PolicyRule::ByPassRoute{}; }

auto PolicyRuleToString(const PolicyRule& rule) -> std::string {
  std::string scopeStr;
  switch (rule.Scope) {
  case PolicyScope::SingleProcess:
    scopeStr = "SingleProcess";
    break;
  case PolicyScope::ProcessSubtree:
    scopeStr = "ProcessSubtree";
    break;
  default:
    scopeStr = "Unknown";
    break;
  }
  return std::format("Rule(Action={}, Scope={})", PolicyActionToString(rule.Action), scopeStr);
}

auto PolicyActionToString(const PolicyRule::RoutingAction& action) -> std::string {
  return std::visit(Overload{
                        [](PolicyRule::ByPassRoute) -> std::string { return "ByPass"; },
                        [](const PolicyRule::EndpointRoute& route) -> std::string {
                          if (auto session = route.Endpoint.lock()) {
                            return std::format("Endpoint[{}]", session->GetDescription());
                          } else {
                            return "Endpoint[Invalid]";
                          }
                        },
                    },
                    action);
}

} // namespace gh::policy
