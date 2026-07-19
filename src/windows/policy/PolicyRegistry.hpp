#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include <windows.h>

#include "Interface.hpp"

namespace gh::policy {

using PolicyScope = Interface::PolicyScope;
using PolicyRule = Interface::PolicyRule;

class PolicyRegistry {
public:
  explicit PolicyRegistry() = default;
  ~PolicyRegistry() = default;

  PolicyRegistry(const PolicyRegistry&) = delete;
  auto operator=(const PolicyRegistry&) -> PolicyRegistry& = delete;
  PolicyRegistry(PolicyRegistry&&) = delete;
  auto operator=(PolicyRegistry&&) -> PolicyRegistry& = delete;

  void Clear();

  void AddPathRule(const std::string& pathPattern, const PolicyRule& rule);
  void RemovePathRule(const std::string& pathPattern);
  [[nodiscard]] auto GetRuleForPath(const std::string& path) const -> std::optional<PolicyRule>;

  void SetDefaultAction(const PolicyRule::RoutingAction& action);
  [[nodiscard]] auto GetDefaultAction() const -> PolicyRule::RoutingAction;

private:
  std::unordered_map<std::string, PolicyRule> _PathRules;
  PolicyRule::RoutingAction _DefaultRoute = PolicyRule::ByPassRoute{};
};

} // namespace gh::policy
