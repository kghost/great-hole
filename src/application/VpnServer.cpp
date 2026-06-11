#include "VpnServer.hpp"

#include <utility>

#include <boost/log/trivial.hpp>

#include "Select.hpp"
#include "SelectPair.hpp"

namespace gh {

VpnServer::VpnServer(std::shared_ptr<EndpointTunSplitIp> tunSplit, std::vector<std::shared_ptr<Filter>> filters)
    : _TunSplit(std::move(tunSplit)), _Filters(std::move(filters)) {}

VpnServer::~VpnServer() {}

void VpnServer::RegisterPeer(const UdpDynMux::PskType& psk, const std::vector<boost::asio::ip::address_v6>& ips) {
  _Peers[psk] = ips;
}

void VpnServer::UnregisterPeer(const UdpDynMux::PskType& psk) { _Peers.erase(psk); }

Omni::Fiber::Coroutine<void> VpnServer::OnChannelEstablished(std::shared_ptr<UdpDynMux::Channel> channel) {
  BOOST_LOG_TRIVIAL(info) << "VpnServer: client channel established: " << channel->GetName();
  co_await _ChannelCall.Call([this, channel = std::move(channel)]() mutable -> Omni::Fiber::Coroutine<void> {
    auto psk = channel->GetPsk();
    auto it = _Peers.find(psk);
    if (it == _Peers.end()) {
      BOOST_LOG_TRIVIAL(warning) << "VpnServer: Unknown client connected (PSK mismatch), ignoring";
      co_return;
    }

    if (_Sessions.contains(channel)) {
      BOOST_LOG_TRIVIAL(warning) << "VpnServer: Client session already exists, cleaning up old session";
      auto& oldSession = _Sessions[channel];
      co_await oldSession.Pipeline->Stop();
      co_await _TunSplit->RemoveChannel(oldSession.TunChannel);
      _Sessions.erase(channel);
    }

    BOOST_LOG_TRIVIAL(info) << "VpnServer: Creating split IP tunnel channel for client";
    auto tunCh = co_await _TunSplit->CreateChannel(it->second);
    if (!tunCh) {
      BOOST_LOG_TRIVIAL(error) << "VpnServer: Failed to create TunSplitIp channel for client";
      co_return;
    }

    BOOST_LOG_TRIVIAL(info) << "VpnServer: Starting bidirectional pipeline";
    auto pipe = std::make_shared<Pipeline>(channel, _Filters, tunCh);

    auto err = co_await pipe->Start();
    if (err) {
      BOOST_LOG_TRIVIAL(error) << "VpnServer: Failed to start pipeline";
      co_await pipe->Stop();
      co_await _TunSplit->RemoveChannel(tunCh);
      co_return;
    }

    _Sessions.emplace(std::move(channel), Session{std::move(tunCh), std::move(pipe)});
  });
}

Omni::Fiber::Coroutine<void> VpnServer::OnChannelClosed(std::shared_ptr<UdpDynMux::Channel> channel) {
  BOOST_LOG_TRIVIAL(info) << "VpnServer: client channel closed: " << channel->GetName();
  co_await _ChannelCall.Call([this, channel = std::move(channel)]() mutable -> Omni::Fiber::Coroutine<void> {
    auto it = _Sessions.find(channel);
    if (it != _Sessions.end()) {
      BOOST_LOG_TRIVIAL(info) << "VpnServer: Cleaning up disconnected client session";
      co_await it->second.Pipeline->Stop();
      co_await _TunSplit->RemoveChannel(it->second.TunChannel);
      _Sessions.erase(it);
    }
  });
}

Omni::Fiber::Coroutine<void> VpnServer::Run(Cancel& c) {
  BOOST_LOG_TRIVIAL(info) << "VpnServer: run loop started";

  bool stopped = false;
  while (!stopped) {
    auto [stopResult, rpcResult] = co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(c.GetFiberCancelEvent(), [] {}),
        Omni::Fiber::SelectPair(_ChannelCall.GetServiceAwaitor(), Omni::Fiber::RemoteCall::HandleRequest));
    if (stopResult) {
      stopped = true;
    }
    if (rpcResult.has_value() && !rpcResult.value()) {
      stopped = true;
    }
  }

  // Cleanup all sessions on stop
  BOOST_LOG_TRIVIAL(info) << "VpnServer: Stopping all client sessions";
  for (auto& [channel, session] : _Sessions) {
    co_await session.Pipeline->Stop();
    co_await _TunSplit->RemoveChannel(session.TunChannel);
  }
  _Sessions.clear();
  co_await _ChannelCall.Close();

  BOOST_LOG_TRIVIAL(info) << "VpnServer: run loop exited";
}

} // namespace gh
