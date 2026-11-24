#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <variant>
#include <vector>

#include "ConnectionTracker.hpp"
#include "InterfaceCommonTypes.hpp"

namespace gh {

class VpnSplitRule {
public:
  using RoutingAction = Interface::PolicyRule::RoutingAction;

  class Callback {
  public:
    explicit Callback() = default;
    virtual ~Callback() = default;

    Callback(const Callback&) = delete;
    auto operator=(const Callback&) -> Callback& = delete;
    Callback(Callback&&) = delete;
    auto operator=(Callback&&) -> Callback& = delete;

    [[nodiscard]] virtual auto Resolve(const ConnectionTracker::ConnectionKey& key) const
        -> std::optional<RoutingAction> = 0;
  };

  explicit VpnSplitRule() = default;
  ~VpnSplitRule() = default;

  VpnSplitRule(const VpnSplitRule&) = delete;
  auto operator=(const VpnSplitRule&) -> VpnSplitRule& = delete;
  VpnSplitRule(VpnSplitRule&&) = delete;
  auto operator=(VpnSplitRule&&) -> VpnSplitRule& = delete;

  void AddCallback(Callback& callback) {
    auto iterator = std::ranges::find_if(
        _Callbacks,
        [&callback](const std::reference_wrapper<Callback>& item) -> bool { return &item.get() == &callback; });
    if (iterator == _Callbacks.end()) {
      _Callbacks.emplace_back(callback);
    }
  }

  void RemoveCallback(Callback& callback) {
    std::erase_if(_Callbacks,
                  [&callback](const std::reference_wrapper<Callback>& item) -> bool { return &item.get() == &callback; });
  }

  [[nodiscard]] auto Resolve(const ConnectionTracker::ConnectionKey& key) const -> RoutingAction {
    for (const auto& callbackRef : _Callbacks) {
      if (auto action = callbackRef.get().Resolve(key); action.has_value()) {
        if (std::holds_alternative<Interface::PolicyRule::VpnSplitRoute>(action.value())) {
          return Interface::PolicyRule::ByPassRoute{};
        }
        return action.value();
      }
    }
    return Interface::PolicyRule::ByPassRoute{};
  }

  [[nodiscard]] static auto ResolveRoute(const Interface::PolicyRule::VpnSplitRoute& route,
                                         const ConnectionTracker::ConnectionKey& key) -> RoutingAction {
    if (auto rules = route.Rules.lock()) {
      return rules->Resolve(key);
    }
    return Interface::PolicyRule::ByPassRoute{};
  }

private:
  std::vector<std::reference_wrapper<Callback>> _Callbacks;
};

} // namespace gh
