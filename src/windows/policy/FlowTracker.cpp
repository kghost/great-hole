#include "FlowTracker.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/log/trivial.hpp>
#include <optional>
#include <variant>

#include "PolicySelector.hpp"
#include "Utils/Overload.hpp"

namespace gh::policy {

FlowTracker::FlowTracker(FlowTrackerDeferredCallback& callback) : _Callback(callback) {}
FlowTracker::~FlowTracker() { _PendingFlowResumers.clear(); }

auto FlowTracker::GetPidForConnection(const ConnectionTracker::ConnectionKey& key) -> std::optional<DWORD> {
  auto flowKey = ToFlowExactKey(key);
  if (flowKey.has_value()) {
    auto iterator = _FlowToPid.find(flowKey.value());
    if (iterator != _FlowToPid.end()) {
      return iterator->second;
    }
  }

  auto flowWildcardKey = ToFlowWildcardKey(key);
  if (flowWildcardKey.has_value()) {
    auto iterator = _FlowToPid.find(flowWildcardKey.value());
    if (iterator != _FlowToPid.end()) {
      return iterator->second;
    }
  }

  // TODO: how to handle icmp?
  return std::nullopt;
}

void FlowTracker::AddPendingMark(const ConnectionTracker::ConnectionKey& key,
                                 const std::shared_ptr<VpnClientMultiChannel::Mark>& mark) {
  std::erase_if(_PendingFlowResumers, [](const auto& item) -> auto { return item.second.expired(); });
  _PendingFlowResumers.emplace_back(key, mark);
}

auto FlowTracker::OnFlowEstablished(const FlowKey& key, uint32_t pid) -> Omni::Fiber::Coroutine<void> {
  auto [iterator, inserted] = _FlowToPid.try_emplace(key, pid);
  if (inserted) {
    auto marks = PickPending(key);
    for (const auto& weak : marks) {
      if (auto mark = weak.lock()) {
        co_await _Callback.FlowTrackerContinue(mark, pid);
      }
    }
  }
  co_return;
}

auto FlowTracker::OnFlowDeleted(const FlowKey& key) -> Omni::Fiber::Coroutine<void> {
  auto removed = _FlowToPid.erase(key);
  if (removed == 0) {
    auto marks = PickPending(key);
  }
  co_return;
}

auto FlowTracker::PickPending(const FlowKey& key) -> std::vector<std::weak_ptr<VpnClientMultiChannel::Mark>> {
  std::vector<decltype(_PendingFlowResumers)::iterator> resolved;
  std::vector<std::weak_ptr<VpnClientMultiChannel::Mark>> marks;
  for (auto iterator = _PendingFlowResumers.begin(); iterator != _PendingFlowResumers.end(); ++iterator) {
    auto& [connKey, weakMark] = *iterator;
    auto exact = ToFlowExactKey(connKey);
    if (exact.has_value() && exact.value() == key) {
      resolved.emplace_back(iterator);
      marks.push_back(iterator->second);
      continue;
    }

    auto wild = ToFlowWildcardKey(connKey);
    if (wild.has_value() && wild.value() == key) {
      resolved.emplace_back(iterator);
      marks.push_back(iterator->second);
      continue;
    }
  }

  for (const auto& iterator : resolved) {
    _PendingFlowResumers.erase(iterator);
  }

  return marks;
}

auto FlowTracker::GetFlows() const -> std::vector<Interface::FlowInfo> {
  std::vector<Interface::FlowInfo> flows;
  flows.reserve(_FlowToPid.size());
  for (const auto& [key, pid] : _FlowToPid) {
    std::visit(Overload{[&](const auto& key) -> void {
                 flows.push_back({.Protocol = ProtocolToString(key.Proto),
                                  .LocalAddress = key.LocalAddress.to_string(),
                                  .LocalPort = key.LocalPort,
                                  .ProcessId = pid});
               }},
               key);
  }
  return flows;
}

auto FlowTracker::GetPendingFlows() const -> std::vector<Interface::PendingFlowInfo> {
  std::vector<Interface::PendingFlowInfo> pending;
  for (const auto& [key, weakMark] : _PendingFlowResumers) {
    if (auto mark = weakMark.lock()) {
      pending.push_back({.Connection = ToFlowConnection(key), .QueueSize = mark->GetPendingQueueSize()});
    }
  }
  return pending;
}

[[nodiscard]] auto FlowTracker::ToFlowExactKey(const ConnectionTracker::ConnectionKey& key) -> std::optional<FlowKey> {
  return std::visit(
      Overload{
          [](const ConnectionTracker::Ip4TcpKey& key) -> std::optional<FlowKey> {
            return FlowIp4Key{.Proto = Protocol::Ipv4Tcp, .LocalAddress = key.LocalAddress, .LocalPort = key.LocalPort};
          },
          [](const ConnectionTracker::Ip6TcpKey& key) -> std::optional<FlowKey> {
            return FlowIp6Key{.Proto = Protocol::Ipv6Tcp, .LocalAddress = key.LocalAddress, .LocalPort = key.LocalPort};
          },
          [](const ConnectionTracker::Ip4UdpKey& key) -> std::optional<FlowKey> {
            return FlowIp4Key{.Proto = Protocol::Ipv4Udp, .LocalAddress = key.LocalAddress, .LocalPort = key.LocalPort};
          },
          [](const ConnectionTracker::Ip6UdpKey& key) -> std::optional<FlowKey> {
            return FlowIp6Key{.Proto = Protocol::Ipv6Udp, .LocalAddress = key.LocalAddress, .LocalPort = key.LocalPort};
          },
          [](const ConnectionTracker::IcmpKey&) -> std::optional<FlowKey> { return std::nullopt; },
          [](const ConnectionTracker::Icmp6Key&) -> std::optional<FlowKey> { return std::nullopt; },
      },
      key);
}

[[nodiscard]] auto FlowTracker::ToFlowWildcardKey(const ConnectionTracker::ConnectionKey& key)
    -> std::optional<FlowKey> {
  return std::visit(Overload{
                        [](const ConnectionTracker::Ip4TcpKey& key) -> std::optional<FlowKey> {
                          return FlowIp4Key{.Proto = Protocol::Ipv4Tcp,
                                            .LocalAddress = boost::asio::ip::address_v4::any(),
                                            .LocalPort = key.LocalPort};
                        },
                        [](const ConnectionTracker::Ip6TcpKey& key) -> std::optional<FlowKey> {
                          return FlowIp6Key{.Proto = Protocol::Ipv6Tcp,
                                            .LocalAddress = boost::asio::ip::address_v6::any(),
                                            .LocalPort = key.LocalPort};
                        },
                        [](const ConnectionTracker::Ip4UdpKey& key) -> std::optional<FlowKey> {
                          return FlowIp4Key{.Proto = Protocol::Ipv4Udp,
                                            .LocalAddress = boost::asio::ip::address_v4::any(),
                                            .LocalPort = key.LocalPort};
                        },
                        [](const ConnectionTracker::Ip6UdpKey& key) -> std::optional<FlowKey> {
                          return FlowIp6Key{.Proto = Protocol::Ipv6Udp,
                                            .LocalAddress = boost::asio::ip::address_v6::any(),
                                            .LocalPort = key.LocalPort};
                        },
                        [](const ConnectionTracker::IcmpKey&) -> std::optional<FlowKey> { return std::nullopt; },
                        [](const ConnectionTracker::Icmp6Key&) -> std::optional<FlowKey> { return std::nullopt; },
                    },
                    key);
}

} // namespace gh::policy