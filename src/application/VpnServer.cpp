#include "VpnServer.hpp"

#include <algorithm>
#include <utility>

#include <boost/log/trivial.hpp>

#include "Select.hpp"

namespace gh {

VpnServer::VpnServer(std::shared_ptr<EndpointTunSplitIp> tunSplit, std::vector<std::shared_ptr<Filter>> filters)
    : _TunSplit(std::move(tunSplit)), _Filters(std::move(filters)) {}

VpnServer::~VpnServer() {}

void VpnServer::RegisterPeer(const UdpDynMux::PskType& psk, const std::vector<boost::asio::ip::address_v6>& ips) {
  _Peers[psk] = ips;
}

void VpnServer::UnregisterPeer(const UdpDynMux::PskType& psk) { _Peers.erase(psk); }

Omni::Fiber::Coroutine<void> VpnServer::OnChannelEstablished(std::shared_ptr<UdpDynMux::Channel> ch) {
  BOOST_LOG_TRIVIAL(info) << "VpnServer: client channel established: " << ch->GetName();
  co_await _Events.GetProducer().Put(Event{EventType::kEstablished, std::move(ch)});
}

Omni::Fiber::Coroutine<void> VpnServer::OnChannelClosed(std::shared_ptr<UdpDynMux::Channel> ch) {
  BOOST_LOG_TRIVIAL(info) << "VpnServer: client channel closed: " << ch->GetName();
  co_await _Events.GetProducer().Put(Event{EventType::kClosed, std::move(ch)});
}

Omni::Fiber::Coroutine<void> VpnServer::Run(Cancel& c) {
  BOOST_LOG_TRIVIAL(info) << "VpnServer: run loop started";

  while (!c.IsTriggered()) {
    Event ev;
    auto [stopped, err] =
        co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(c.GetFiberCancelEvent(), [] {}),
                                     Omni::Fiber::SelectPair(_Events.GetConsumer(), [&](auto val) -> ErrorCode {
                                       if (val.has_value()) {
                                         ev = std::move(val.value());
                                         return ErrorCode{};
                                       } else {
                                         return ErrorCode{AppErrorCategory::kEndOfStream, kAppError};
                                       }
                                     }));

    if (stopped || err) {
      break;
    }

    if (ev.Type == EventType::kEstablished) {
      auto psk = ev.Channel->GetPsk();
      auto it = _Peers.find(psk);
      if (it == _Peers.end()) {
        BOOST_LOG_TRIVIAL(warning) << "VpnServer: Unknown client connected (PSK mismatch), ignoring";
        continue;
      }

      if (_Sessions.contains(ev.Channel)) {
        BOOST_LOG_TRIVIAL(warning) << "VpnServer: Client session already exists, cleaning up old session";
        auto& oldSession = _Sessions[ev.Channel];
        co_await oldSession.Pipe->Stop();
        co_await _TunSplit->RemoveChannel(oldSession.TunChannel);
        _Sessions.erase(ev.Channel);
      }

      BOOST_LOG_TRIVIAL(info) << "VpnServer: Creating split IP tunnel channel for client";
      auto tunCh = co_await _TunSplit->CreateChannel(it->second);
      if (!tunCh) {
        BOOST_LOG_TRIVIAL(error) << "VpnServer: Failed to create TunSplitIp channel for client";
        continue;
      }

      BOOST_LOG_TRIVIAL(info) << "VpnServer: Starting bidirectional pipeline";
      auto pipe = std::make_shared<Pipeline>(ev.Channel, _Filters, tunCh);

      auto err = co_await pipe->Start();
      if (err) {
        BOOST_LOG_TRIVIAL(error) << "VpnServer: Failed to start pipeline";
        co_await pipe->Stop();
        co_await _TunSplit->RemoveChannel(tunCh);
        continue;
      }

      _Sessions.emplace(ev.Channel, Session{std::move(tunCh), std::move(pipe)});
    } else if (ev.Type == EventType::kClosed) {
      auto it = _Sessions.find(ev.Channel);
      if (it != _Sessions.end()) {
        BOOST_LOG_TRIVIAL(info) << "VpnServer: Cleaning up disconnected client session";
        co_await it->second.Pipe->Stop();
        co_await _TunSplit->RemoveChannel(it->second.TunChannel);
        _Sessions.erase(it);
      }
    }
  }

  // Cleanup all sessions on stop
  BOOST_LOG_TRIVIAL(info) << "VpnServer: Stopping all client sessions";
  for (auto& [ch, session] : _Sessions) {
    co_await session.Pipe->Stop();
    co_await _TunSplit->RemoveChannel(session.TunChannel);
  }
  _Sessions.clear();
  co_await _Events.GetProducer().Close();

  BOOST_LOG_TRIVIAL(info) << "VpnServer: run loop exited";
}

} // namespace gh
