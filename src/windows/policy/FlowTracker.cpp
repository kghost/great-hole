#include "FlowTracker.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/log/trivial.hpp>
#include <optional>

namespace gh::policy {

FlowTracker::FlowTracker(FlowTrackerDeferredCallback& callback) : _Callback(callback) {}
FlowTracker::~FlowTracker() { _PendingFlowResumers.clear(); }

auto FlowTracker::GetPidForConnection(const ConnectionTracker::ConnectionKey& key) -> std::optional<DWORD> {
  auto iterator = _FlowToPid.find(key);
  if (iterator != _FlowToPid.end()) {
    return iterator->second;
  } else {
    return std::nullopt;
  }
}

void FlowTracker::AddPendingMark(const ConnectionTracker::ConnectionKey& key,
                                 const std::shared_ptr<VpnClientMultiChannel::Mark>& mark) {
  _PendingFlowResumers.try_emplace(key, mark);
}

auto FlowTracker::OnFlowEstablished(const ConnectionTracker::ConnectionKey& conn, uint32_t pid)
    -> Omni::Fiber::Coroutine<void> {
  if (std::holds_alternative<ConnectionTracker::IcmpKey>(conn)) {
    BOOST_LOG_TRIVIAL(info) << "FlowTracker: ICMPv4 flow established: " << conn << ", PID: " << pid;
  } else if (std::holds_alternative<ConnectionTracker::Icmp6Key>(conn)) {
    BOOST_LOG_TRIVIAL(info) << "FlowTracker: ICMPv6 flow established: " << conn << ", PID: " << pid;
  }

  auto [iterator, inserted] = _FlowToPid.try_emplace(conn, pid);
  if (inserted) {
    if (auto resumerIter = _PendingFlowResumers.find(conn); resumerIter != _PendingFlowResumers.end()) {
      auto mark = std::move(resumerIter->second);
      _PendingFlowResumers.erase(resumerIter);
      co_await _Callback.FlowTrackerContinue(mark, pid);
    }
  }
  co_return;
}

auto FlowTracker::OnFlowDeleted(const ConnectionTracker::ConnectionKey& conn) -> Omni::Fiber::Coroutine<void> {
  auto removed = _FlowToPid.erase(conn);
  if (removed == 0) {
    _PendingFlowResumers.erase(conn);
  }
  co_return;
}

namespace {

auto ToFlowConnection(const ConnectionTracker::ConnectionKey& key) -> Interface::FlowConnection {
  Interface::FlowConnection conn;
  std::visit(
      [&conn](const auto& key) -> void {
        using T = std::decay_t<decltype(key)>;
        if constexpr (std::is_same_v<T, ConnectionTracker::Ip4TcpKey>) {
          conn.Protocol = "TCPv4";
          conn.LocalAddress = key.LocalAddress.to_string();
          conn.LocalPort = key.LocalPort;
          conn.RemoteAddress = key.RemoteAddress.to_string();
          conn.RemotePort = key.RemotePort;
        } else if constexpr (std::is_same_v<T, ConnectionTracker::Ip6TcpKey>) {
          conn.Protocol = "TCPv6";
          conn.LocalAddress = key.LocalAddress.to_string();
          conn.LocalPort = key.LocalPort;
          conn.RemoteAddress = key.RemoteAddress.to_string();
          conn.RemotePort = key.RemotePort;
        } else if constexpr (std::is_same_v<T, ConnectionTracker::Ip4UdpKey>) {
          conn.Protocol = "UDPv4";
          conn.LocalAddress = key.LocalAddress.to_string();
          conn.LocalPort = key.LocalPort;
          conn.RemoteAddress = key.RemoteAddress.to_string();
          conn.RemotePort = key.RemotePort;
        } else if constexpr (std::is_same_v<T, ConnectionTracker::Ip6UdpKey>) {
          conn.Protocol = "UDPv6";
          conn.LocalAddress = key.LocalAddress.to_string();
          conn.LocalPort = key.LocalPort;
          conn.RemoteAddress = key.RemoteAddress.to_string();
          conn.RemotePort = key.RemotePort;
        } else if constexpr (std::is_same_v<T, ConnectionTracker::IcmpKey>) {
          conn.Protocol = "ICMPv4";
          conn.LocalAddress = key.LocalAddress.to_string();
          conn.RemoteAddress = key.RemoteAddress.to_string();
          conn.LocalPort = key.Id;
          conn.RemotePort = 0;
        } else if constexpr (std::is_same_v<T, ConnectionTracker::Icmp6Key>) {
          conn.Protocol = "ICMPv6";
          conn.LocalAddress = key.LocalAddress.to_string();
          conn.RemoteAddress = key.RemoteAddress.to_string();
          conn.LocalPort = key.Id;
          conn.RemotePort = 0;
        }
      },
      key);
  return conn;
}

} // namespace

auto FlowTracker::GetFlows() const -> std::vector<Interface::FlowInfo> {
  std::vector<Interface::FlowInfo> flows;
  flows.reserve(_FlowToPid.size());
  for (const auto& [key, pid] : _FlowToPid) {
    flows.push_back({.Connection = ToFlowConnection(key), .ProcessId = pid});
  }
  return flows;
}

auto FlowTracker::GetPendingFlows() const -> std::vector<Interface::PendingFlowInfo> {
  std::vector<Interface::PendingFlowInfo> pending;
  pending.reserve(_PendingFlowResumers.size());
  for (const auto& [key, mark] : _PendingFlowResumers) {
    pending.push_back({.Connection = ToFlowConnection(key), .QueueSize = mark->GetPendingQueueSize()});
  }
  return pending;
}

} // namespace gh::policy
