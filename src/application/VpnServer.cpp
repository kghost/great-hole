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
  auto endpoint = std::make_shared<Endpoint>(psk, ips);
  _Endpoints[psk] = endpoint;
  if (_State == State::kRunning && _UdpDynMux) {
    endpoint->UdpChannel = co_await _UdpDynMux->CreateChannel(psk, *endpoint);
  }
  co_return;
}

auto VpnServer::UnregisterPeer(const UdpDynMux::PskType& psk) -> Omni::Fiber::Coroutine<void> {
  if (auto iterator = _Endpoints.find(psk); iterator != _Endpoints.end()) {
    auto endpoint = std::move(iterator->second);
    _Endpoints.erase(iterator);
    if (_State == State::kRunning && _UdpDynMux && endpoint->UdpChannel) {
      co_await _UdpDynMux->RemoveChannel(endpoint->UdpChannel);
    }
  }
  co_return;
}

auto VpnServer::OnChannelEstablished(UdpDynMux::ChannelNotificationTarget& target) -> Omni::Fiber::Coroutine<void> {
  auto& endpoint = dynamic_cast<VpnServer::Endpoint&>(target);
  BOOST_LOG_TRIVIAL(info) << "VpnServer: client channel established: " << endpoint.UdpChannel->GetName();
  co_await _ChannelCall.Call([this, &endpoint]() mutable -> Omni::Fiber::Coroutine<void> {
    if (endpoint.SessionPipeline) {
      BOOST_LOG_TRIVIAL(warning) << "VpnServer: Client session already exists, cleaning up old session";
      co_await endpoint.SessionPipeline->Stop();
      endpoint.SessionPipeline.reset();
      if (endpoint.TunChannel) {
        co_await _TunSplit->RemoveChannel(endpoint.TunChannel);
        endpoint.TunChannel.reset();
      }
    }

    BOOST_LOG_TRIVIAL(info) << "VpnServer: Creating split IP tunnel channel for client";
    auto tunChannel = co_await _TunSplit->CreateChannel(endpoint.Ips);
    if (!tunChannel) {
      BOOST_LOG_TRIVIAL(error) << "VpnServer: Failed to create TunSplitIp channel for client";
      co_return;
    }

    BOOST_LOG_TRIVIAL(info) << "VpnServer: Starting bidirectional pipeline";
    auto pipe = std::make_shared<Pipeline>(endpoint.UdpChannel, _Filters, tunChannel);

    auto err = co_await pipe->Start();
    if (err) {
      BOOST_LOG_TRIVIAL(error) << "VpnServer: Failed to start pipeline";
      co_await pipe->Stop();
      co_await _TunSplit->RemoveChannel(tunChannel);
      co_return;
    }

    endpoint.TunChannel = std::move(tunChannel);
    endpoint.SessionPipeline = std::move(pipe);
  });
}

auto VpnServer::OnChannelClosed(UdpDynMux::ChannelNotificationTarget& target) -> Omni::Fiber::Coroutine<void> {
  auto& endpoint = dynamic_cast<VpnServer::Endpoint&>(target);
  BOOST_LOG_TRIVIAL(info) << "VpnServer: client channel closed: " << endpoint.UdpChannel->GetName();
  co_await _ChannelCall.Call([this, &endpoint]() mutable -> Omni::Fiber::Coroutine<void> {
    if (endpoint.SessionPipeline) {
      BOOST_LOG_TRIVIAL(info) << "VpnServer: Cleaning up disconnected client session";
      co_await endpoint.SessionPipeline->Stop();
      endpoint.SessionPipeline.reset();
    }
    if (endpoint.TunChannel) {
      co_await _TunSplit->RemoveChannel(endpoint.TunChannel);
      endpoint.TunChannel.reset();
    }
  });
}

auto VpnServer::GetName() const -> std::string {
  return std::format("VpnServer:[{}-{}]", _UdpDynMux->GetName(), _TunSplit->GetName());
}

auto VpnServer::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> {
  if (_UdpDynMux) {
    for (auto const& [psk, endpoint] : _Endpoints) {
      endpoint->UdpChannel = co_await _UdpDynMux->CreateChannel(psk, *endpoint);
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
  for (auto& [psk, endpoint] : _Endpoints) {
    if (endpoint->SessionPipeline) {
      co_await endpoint->SessionPipeline->Stop();
      endpoint->SessionPipeline.reset();
    }
    if (endpoint->TunChannel) {
      co_await _TunSplit->RemoveChannel(endpoint->TunChannel);
      endpoint->TunChannel.reset();
    }
  }
  _ChannelCall.DiscardAndClose();

  BOOST_LOG_TRIVIAL(info) << "VpnServer: run loop exited";
  co_return ErrorCode{};
}

} // namespace gh
