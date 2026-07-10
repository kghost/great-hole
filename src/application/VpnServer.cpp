#include "VpnServer.hpp"

#include <format>
#include <memory>
#include <utility>

#include <boost/asio/ip/address_v6.hpp>
#include <boost/log/trivial.hpp>

#include "Select.hpp"
#include "SelectPair.hpp"

namespace gh {

VpnServer::VpnServer(std::shared_ptr<EndpointTunSplitIp> tunSplit, std::shared_ptr<UdpDynMux> udpDynMux,
                     std::vector<std::shared_ptr<Filter>> filters)
    : _TunSplit(std::move(tunSplit)), _UdpDynMux(std::move(udpDynMux)), _Filters(std::move(filters)) {
  _UdpDynMux->SetChannelNotification(*this);
}

VpnServer::~VpnServer() {}

auto VpnServer::RegisterPeer(const UdpDynMux::PskType& psk, const std::vector<boost::asio::ip::address_v6>& ips)
    -> Omni::Fiber::Coroutine<void> {
  _Peers[psk] = ips;
  if (_State == State::kRunning && _UdpDynMux) {
    co_await _UdpDynMux->CreateChannel(psk);
  }
  co_return;
}

auto VpnServer::UnregisterPeer(const UdpDynMux::PskType& psk) -> Omni::Fiber::Coroutine<void> {
  _Peers.erase(psk);
  if (_State == State::kRunning && _UdpDynMux) {
    co_await _UdpDynMux->RemoveChannel(psk);
  }
  co_return;
}

auto VpnServer::OnChannelEstablished(std::shared_ptr<UdpDynMux::Channel> channel) -> Omni::Fiber::Coroutine<void> {
  BOOST_LOG_TRIVIAL(info) << "VpnServer: client channel established: " << channel->GetName();
  co_await _ChannelCall.Call([this, channel = std::move(channel)]() mutable -> Omni::Fiber::Coroutine<void> {
    auto psk = channel->GetPsk();
    auto iterator = _Peers.find(psk);
    if (iterator == _Peers.end()) {
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
    auto tunChannel = co_await _TunSplit->CreateChannel(iterator->second);
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

    _Sessions.emplace(std::move(channel),
                      Session{.TunChannel = std::move(tunChannel), .SessionPipeline = std::move(pipe)});
  });
}

auto VpnServer::OnChannelClosed(std::shared_ptr<UdpDynMux::Channel> channel) -> Omni::Fiber::Coroutine<void> {
  BOOST_LOG_TRIVIAL(info) << "VpnServer: client channel closed: " << channel->GetName();
  co_await _ChannelCall.Call([this, channel = std::move(channel)]() mutable -> Omni::Fiber::Coroutine<void> {
    auto iterator = _Sessions.find(channel);
    if (iterator != _Sessions.end()) {
      BOOST_LOG_TRIVIAL(info) << "VpnServer: Cleaning up disconnected client session";
      co_await iterator->second.SessionPipeline->Stop();
      co_await _TunSplit->RemoveChannel(iterator->second.TunChannel);
      _Sessions.erase(iterator);
    }
  });
}

auto VpnServer::GetName() const -> std::string {
  return std::format("VpnServer:[{}-{}]", _UdpDynMux->GetName(), _TunSplit->GetName());
}

auto VpnServer::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> {
  if (_UdpDynMux) {
    for (auto const& [psk, ips] : _Peers) {
      co_await _UdpDynMux->CreateChannel(psk);
    }
  }
  co_return ErrorCode{};
}

auto VpnServer::DoWork() -> Omni::Fiber::Coroutine<void> {
  BOOST_LOG_TRIVIAL(info) << "VpnServer: run loop started";

  bool stopped = false;
  while (!stopped) {
    auto [stopResult, rpcResult] = co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(), [] -> void {}),
        Omni::Fiber::SelectPair(_ChannelCall.GetServiceAwaitor(), Omni::Fiber::RemoteCall::HandleRequest));
    if (stopResult) {
      stopped = true;
    }
    if (rpcResult.has_value() && !rpcResult.value()) {
      stopped = true;
    }
  }
}

auto VpnServer::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> {
  // Cleanup all sessions on stop
  BOOST_LOG_TRIVIAL(info) << "VpnServer: Stopping all client sessions";
  for (auto& [channel, session] : _Sessions) {
    co_await session.SessionPipeline->Stop();
    co_await _TunSplit->RemoveChannel(session.TunChannel);
  }
  _Sessions.clear();
  _ChannelCall.DiscardAndClose();

  BOOST_LOG_TRIVIAL(info) << "VpnServer: run loop exited";
  co_return ErrorCode{};
}

} // namespace gh
