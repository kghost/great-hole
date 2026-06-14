#include "VpnServer.hpp"

#include <format>
#include <utility>

#include <boost/log/trivial.hpp>

#include "Select.hpp"
#include "SelectPair.hpp"

namespace gh {

VpnServer::VpnServer(std::shared_ptr<EndpointTunSplitIp> tunSplit, std::shared_ptr<UdpDynMux> udpDynMux,
                     std::vector<std::shared_ptr<Filter>> filters)
    : _TunSplit(tunSplit), _UdpDynMux(udpDynMux), _Filters(filters) {
  udpDynMux->SetChannelNotification(*this);
}

VpnServer::~VpnServer() {}

Omni::Fiber::Coroutine<void> VpnServer::RegisterPeer(const UdpDynMux::PskType& psk,
                                                     const std::vector<boost::asio::ip::address_v6>& ips) {
  _Peers[psk] = ips;
  if (_State == State::kRunning && _UdpDynMux) {
    co_await _UdpDynMux->CreateChannel(psk);
  }
  co_return;
}

Omni::Fiber::Coroutine<void> VpnServer::UnregisterPeer(const UdpDynMux::PskType& psk) {
  _Peers.erase(psk);
  if (_State == State::kRunning && _UdpDynMux) {
    co_await _UdpDynMux->RemoveChannel(psk);
  }
  co_return;
}

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
      co_await oldSession.SessionPipeline->Stop();
      co_await _TunSplit->RemoveChannel(oldSession.TunChannel);
      _Sessions.erase(channel);
    }

    BOOST_LOG_TRIVIAL(info) << "VpnServer: Creating split IP tunnel channel for client";
    auto tunChannel = co_await _TunSplit->CreateChannel(it->second);
    if (!tunChannel) {
      BOOST_LOG_TRIVIAL(error) << "VpnServer: Failed to create TunSplitIp channel for client";
      co_return;
    }

    BOOST_LOG_TRIVIAL(info) << "VpnServer: Starting bidirectional pipeline";
    auto pipe = std::make_shared<Pipeline>(channel, _Filters, tunChannel);

    auto err = co_await pipe->Start();
    if (err) {
      BOOST_LOG_TRIVIAL(error) << "VpnServer: Failed to start pipeline";
      co_await pipe->Stop();
      co_await _TunSplit->RemoveChannel(tunChannel);
      co_return;
    }

    _Sessions.emplace(std::move(channel), Session{std::move(tunChannel), std::move(pipe)});
  });
}

Omni::Fiber::Coroutine<void> VpnServer::OnChannelClosed(std::shared_ptr<UdpDynMux::Channel> channel) {
  BOOST_LOG_TRIVIAL(info) << "VpnServer: client channel closed: " << channel->GetName();
  co_await _ChannelCall.Call([this, channel = std::move(channel)]() mutable -> Omni::Fiber::Coroutine<void> {
    auto it = _Sessions.find(channel);
    if (it != _Sessions.end()) {
      BOOST_LOG_TRIVIAL(info) << "VpnServer: Cleaning up disconnected client session";
      co_await it->second.SessionPipeline->Stop();
      co_await _TunSplit->RemoveChannel(it->second.TunChannel);
      _Sessions.erase(it);
    }
  });
}

std::string VpnServer::GetName() const {
  return std::format("VpnServer:[{}-{}]", _UdpDynMux->GetName(), _TunSplit->GetName());
}

Omni::Fiber::Coroutine<ErrorCode> VpnServer::DoStart() {
  if (_UdpDynMux) {
    for (auto const& [psk, ips] : _Peers) {
      co_await _UdpDynMux->CreateChannel(psk);
    }
  }
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<void> VpnServer::DoWork() {
  BOOST_LOG_TRIVIAL(info) << "VpnServer: run loop started";

  bool stopped = false;
  while (!stopped) {
    auto [stopResult, rpcResult] = co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(), [] {}),
        Omni::Fiber::SelectPair(_ChannelCall.GetServiceAwaitor(), Omni::Fiber::RemoteCall::HandleRequest));
    if (stopResult) {
      stopped = true;
    }
    if (rpcResult.has_value() && !rpcResult.value()) {
      stopped = true;
    }
  }
}

Omni::Fiber::Coroutine<ErrorCode> VpnServer::DoGracefulStop() {
  // Cleanup all sessions on stop
  BOOST_LOG_TRIVIAL(info) << "VpnServer: Stopping all client sessions";
  for (auto& [channel, session] : _Sessions) {
    co_await session.SessionPipeline->Stop();
    co_await _TunSplit->RemoveChannel(session.TunChannel);
  }
  _Sessions.clear();
  co_await _ChannelCall.Close();

  BOOST_LOG_TRIVIAL(info) << "VpnServer: run loop exited";
  co_return ErrorCode{};
}

} // namespace gh
