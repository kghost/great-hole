#include "FlowTracker.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/log/trivial.hpp>
#include <optional>
#include <variant>

#include "Utils/Overload.hpp"

namespace gh::policy {

FlowTracker::FlowTracker(FlowTrackerDeferredCallback& callback) : _Callback(callback) {}
FlowTracker::~FlowTracker() { _PendingFlowResumers.clear(); }

auto FlowTracker::GetPidForConnection(const ConnectionTracker::ConnectionKey& key) -> std::optional<DWORD> {
  auto flowKey = ToFlowKey(key);
  if (flowKey.has_value()) {
    auto iterator = _FlowToPid.find(flowKey.value());
    if (iterator != _FlowToPid.end()) {
      return iterator->second;
    } else {
      return std::nullopt;
    }
  } else {
    // TODO: how to handle icmp?
    return std::nullopt;
  }
}

void FlowTracker::AddPendingMark(const ConnectionTracker::ConnectionKey& key,
                                 const std::shared_ptr<VpnClientMultiChannel::Mark>& mark) {
  auto flowKey = ToFlowKey(key);
  if (flowKey.has_value()) {
    _PendingFlowResumers[flowKey.value()].emplace_back(key, mark);
  }
}

auto FlowTracker::OnFlowEstablished(FlowKey key, uint32_t pid) -> Omni::Fiber::Coroutine<void> {
  auto [iterator, inserted] = _FlowToPid.try_emplace(key, pid);
  if (inserted) {
    std::vector<std::pair<ConnectionTracker::ConnectionKey, std::shared_ptr<VpnClientMultiChannel::Mark>>> resolved;

    if (auto resumerIter = _PendingFlowResumers.find(key); resumerIter != _PendingFlowResumers.end()) {
      resolved = std::move(resumerIter->second);
      _PendingFlowResumers.erase(resumerIter);
    }

    for (auto& [connKey, mark] : resolved) {
      co_await _Callback.FlowTrackerContinue(mark, pid);
    }
  }
  co_return;
}

auto FlowTracker::OnFlowDeleted(FlowKey key) -> Omni::Fiber::Coroutine<void> {
  auto removed = _FlowToPid.erase(key);
  if (removed == 0) {
    _PendingFlowResumers.erase(key);
  }
  co_return;
}

namespace {

auto ToFlowConnection(const ConnectionTracker::ConnectionKey& key) -> Interface::FlowConnection {
  return std::visit(Overload{
                        [](const ConnectionTracker::Ip4TcpKey& key) -> Interface::FlowConnection {
                          return {.Protocol = "TCPv4",
                                  .LocalAddress = key.LocalAddress.to_string(),
                                  .RemoteAddress = key.RemoteAddress.to_string(),
                                  .LocalPort = key.LocalPort,
                                  .RemotePort = key.RemotePort};
                        },
                        [](const ConnectionTracker::Ip6TcpKey& key) -> Interface::FlowConnection {
                          return {.Protocol = "TCPv6",
                                  .LocalAddress = key.LocalAddress.to_string(),
                                  .RemoteAddress = key.RemoteAddress.to_string(),
                                  .LocalPort = key.LocalPort,
                                  .RemotePort = key.RemotePort};
                        },
                        [](const ConnectionTracker::Ip4UdpKey& key) -> Interface::FlowConnection {
                          return {.Protocol = "UDPv4",
                                  .LocalAddress = key.LocalAddress.to_string(),
                                  .RemoteAddress = key.RemoteAddress.to_string(),
                                  .LocalPort = key.LocalPort,
                                  .RemotePort = key.RemotePort};
                        },
                        [](const ConnectionTracker::Ip6UdpKey& key) -> Interface::FlowConnection {
                          return {.Protocol = "UDPv6",
                                  .LocalAddress = key.LocalAddress.to_string(),
                                  .RemoteAddress = key.RemoteAddress.to_string(),
                                  .LocalPort = key.LocalPort,
                                  .RemotePort = key.RemotePort};
                        },
                        [](const ConnectionTracker::IcmpKey& key) -> Interface::FlowConnection {
                          return {.Protocol = "ICMPv4",
                                  .LocalAddress = key.LocalAddress.to_string(),
                                  .RemoteAddress = key.RemoteAddress.to_string(),
                                  .LocalPort = 0,
                                  .RemotePort = 0};
                        },
                        [](const ConnectionTracker::Icmp6Key& key) -> Interface::FlowConnection {
                          return {.Protocol = "ICMPv6",
                                  .LocalAddress = key.LocalAddress.to_string(),
                                  .RemoteAddress = key.RemoteAddress.to_string(),
                                  .LocalPort = 0,
                                  .RemotePort = 0};
                        },
                    },
                    key);
}

} // namespace

auto FlowTracker::GetFlows() const -> std::vector<Interface::FlowInfo> {
  std::vector<Interface::FlowInfo> flows;
  flows.reserve(_FlowToPid.size());
  for (const auto& [key, pid] : _FlowToPid) {
    flows.push_back({.Protocol = ProtocolToString(key.Proto), .LocalPort = key.LocalPort, .ProcessId = pid});
  }
  return flows;
}

auto FlowTracker::GetPendingFlows() const -> std::vector<Interface::PendingFlowInfo> {
  std::vector<Interface::PendingFlowInfo> pending;
  for (const auto& [port, vec] : _PendingFlowResumers) {
    for (const auto& [key, mark] : vec) {
      pending.push_back({.Connection = ToFlowConnection(key), .QueueSize = mark->GetPendingQueueSize()});
    }
  }
  return pending;
}

[[nodiscard]] auto FlowTracker::ToFlowKey(const ConnectionTracker::ConnectionKey& key) -> std::optional<FlowKey> {
  return std::visit(Overload{
                        [](const ConnectionTracker::Ip4TcpKey& key) -> std::optional<FlowKey> {
                          return FlowKey{.Proto = Protocol::Ipv4Tcp, .LocalPort = key.LocalPort};
                        },
                        [](const ConnectionTracker::Ip6TcpKey& key) -> std::optional<FlowKey> {
                          return FlowKey{.Proto = Protocol::Ipv6Tcp, .LocalPort = key.LocalPort};
                        },
                        [](const ConnectionTracker::Ip4UdpKey& key) -> std::optional<FlowKey> {
                          return FlowKey{.Proto = Protocol::Ipv4Udp, .LocalPort = key.LocalPort};
                        },
                        [](const ConnectionTracker::Ip6UdpKey& key) -> std::optional<FlowKey> {
                          return FlowKey{.Proto = Protocol::Ipv6Udp, .LocalPort = key.LocalPort};
                        },
                        [](const ConnectionTracker::IcmpKey&) -> std::optional<FlowKey> { return std::nullopt; },
                        [](const ConnectionTracker::Icmp6Key&) -> std::optional<FlowKey> { return std::nullopt; },
                    },
                    key);
}

} // namespace gh::policy