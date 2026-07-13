#pragma once

#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

#include <windows.h>

#include "Interface.hpp"
#include "Utils/Overload.hpp"
#include "VpnClientMultiChannel.hpp"

namespace gh::policy {

using PolicyScope = Interface::PolicyScope;

struct PolicyRule {
  struct ByPassRoute {};
  struct EndpointRoute {
    Interface::VpnEndpoint Endpoint;
  };

  using RoutingAction = std::variant<ByPassRoute, EndpointRoute>;
  [[nodiscard]] auto ToString() const -> std::string {
    return std::visit(Overload{
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

  RoutingAction Action;
  PolicyScope Scope = PolicyScope::SingleProcess;
};

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
