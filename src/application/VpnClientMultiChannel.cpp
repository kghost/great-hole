#include "VpnClientMultiChannel.hpp"

#include <array>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/any_io_executor.hpp>

#include "Cancel.hpp"
#include "ConnectionTracker.hpp"
#include "Coroutine.hpp"
#include "Endpoint.hpp"
#include "EndpointUdpDynMux.hpp"
#include "ErrorCode.hpp"
#include "Packet.hpp"
#include "Pipe.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"
#include "ServiceBase.hpp"
#include "Utils/Overload.hpp"

namespace gh {

class SessionSelector : public ConnectionTracker::Selector {
public:
  explicit SessionSelector(std::shared_ptr<VpnClientMultiChannel::Session> session) : _Session(std::move(session)) {}
  std::unique_ptr<ConnectionMark> operator()(const ConnectionTracker::Ip4TcpKey&) const override {
    return std::make_unique<VpnClientMultiChannel::Mark>(_Session);
  }
  std::unique_ptr<ConnectionMark> operator()(const ConnectionTracker::Ip6TcpKey&) const override {
    return std::make_unique<VpnClientMultiChannel::Mark>(_Session);
  }
  std::unique_ptr<ConnectionMark> operator()(const ConnectionTracker::Ip4UdpKey&) const override {
    return std::make_unique<VpnClientMultiChannel::Mark>(_Session);
  }
  std::unique_ptr<ConnectionMark> operator()(const ConnectionTracker::Ip6UdpKey&) const override {
    return std::make_unique<VpnClientMultiChannel::Mark>(_Session);
  }
  std::unique_ptr<ConnectionMark> operator()(const ConnectionTracker::IcmpKey&) const override {
    return std::make_unique<VpnClientMultiChannel::Mark>(_Session);
  }
  std::unique_ptr<ConnectionMark> operator()(const ConnectionTracker::Icmp6Key&) const override {
    return std::make_unique<VpnClientMultiChannel::Mark>(_Session);
  }

private:
  std::shared_ptr<VpnClientMultiChannel::Session> _Session;
};

std::string VpnClientMultiChannel::Mark::GetDescription() const {
  return std::visit(Overload{[](const ToBeSelected&) -> std::string { return "ToBeSelected"; },
                             [](const Bypass&) -> std::string { return "Bypass"; },
                             [](const Discard&) -> std::string { return "Discard"; },
                             [](const std::weak_ptr<Session>& session) -> std::string {
                               if (auto s = session.lock()) {
                                 return s->GetDescription();
                               }
                               return "Expired Session";
                             }},
                    _Value);
}

bool VpnClientMultiChannel::Mark::Validate() const {
  return std::visit(Overload{[](const ToBeSelected&) { return false; }, [](const Bypass&) { return true; },
                             [](const Discard&) { return true; },
                             [](const std::weak_ptr<Session>& session) {
                               if (auto s = session.lock()) {
                                 return s->Running;
                               }
                               return false;
                             }},
                    _Value);
}

class VpnClientMultiChannel::ChannelSideEndpoint : public Endpoint {
public:
  ChannelSideEndpoint(VpnClientMultiChannel& parent, std::shared_ptr<Session> session)
      : _Parent(parent), _Session(std::move(session)) {}
  ~ChannelSideEndpoint() override = default;

  std::string GetName() const override {
    return std::format("VpnClientMultiChannel:ChannelSide:[{}]", _Session->Channel->GetName());
  }

  Omni::Fiber::Coroutine<ErrorCode> Read(Packet& p, Cancel& c) override {
    if (c.IsTriggered()) {
      co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
    }
    auto [cancelFired, queueFired] =
        co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(c.GetFiberCancelEvent(), [] {}),
                                     Omni::Fiber::SelectPair(_Pipe.GetConsumer(), [&p](auto pkt) -> ErrorCode {
                                       if (pkt.has_value()) {
                                         p = std::move(pkt.value());
                                         return ErrorCode{};
                                       } else {
                                         return ErrorCode{AppErrorCategory::kEndOfStream, kAppError};
                                       }
                                     }));
    if (queueFired.has_value()) {
      co_return queueFired.value();
    }
    if (cancelFired) {
      co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
    }
    assert(false && "should not reach here");
    co_return ErrorCode{};
  }

  Omni::Fiber::Coroutine<ErrorCode> Write(Packet& packet, Cancel& c) override;

  Omni::Fiber::Coroutine<std::expected<void, Omni::Fiber::PipeClosed>> PushOutgoing(Packet p) {
    co_return co_await _Pipe.GetProducer().Put(std::move(p));
  }

protected:
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override { co_return ErrorCode{}; }
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override {
    _Pipe.GetConsumer().DiscardAndClose();
    co_return ErrorCode{};
  }

private:
  VpnClientMultiChannel& _Parent;
  std::shared_ptr<Session> _Session;
  Omni::Fiber::Pipe<Packet> _Pipe;
};

class VpnClientMultiChannel::TunSideEndpoint : public Endpoint {
public:
  struct QueueEntry {
    Packet PacketData;
    std::shared_ptr<Session> PacketSession;
  };

  explicit TunSideEndpoint(VpnClientMultiChannel& parent, ConnectionTracker& tracker,
                           ConnectionTracker::Selector& selector)
      : _Parent(parent), _ConnectionTracker(tracker), _Selector(selector) {}
  ~TunSideEndpoint() override = default;

  std::string GetName() const override { return std::format("VpnClientMultiChannel:TunSide:{}", _Parent.GetName()); }

  Omni::Fiber::Coroutine<std::expected<void, Omni::Fiber::PipeClosed>> PushIncoming(Packet packet,
                                                                                    std::shared_ptr<Session> session) {
    co_return co_await _Pipe.GetProducer().Put(QueueEntry{.PacketData = std::move(packet), .PacketSession = session});
  }

  Omni::Fiber::Coroutine<ErrorCode> Read(Packet& packet, Cancel& c) override {
    if (c.IsTriggered()) {
      co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
    }
    auto [cancelFired, queueFired] = co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(c.GetFiberCancelEvent(), [] {}),
        Omni::Fiber::SelectPair(_Pipe.GetConsumer(), [&packet, this](auto&& entry) -> ErrorCode {
          if (entry.has_value()) {
            packet = std::move(entry.value().PacketData);
            SessionSelector sessionSelector(entry.value().PacketSession);
            auto res = _ConnectionTracker.LookupAndUpdate<ConnectionTracker::ConnectionDirectionInput>(
                packet, sessionSelector);
            if (!res) {
              BOOST_LOG_TRIVIAL(warning) << GetName()
                                         << ": LookupAndUpdate on input path failed: " << res.error().message();
            }
            return ErrorCode{};
          } else {
            return ErrorCode{AppErrorCategory::kEndOfStream, kAppError};
          }
        }));
    if (queueFired.has_value()) {
      co_return queueFired.value();
    }
    if (cancelFired) {
      co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
    }
    assert(false && "should not reach here");
    co_return ErrorCode{};
  }

  Omni::Fiber::Coroutine<ErrorCode> Write(Packet& packet, Cancel& c) override {
    if (c.IsTriggered()) {
      co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
    }

    auto route = _ConnectionTracker.LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(
        packet, _Selector);

    if (!route) {
      BOOST_LOG_TRIVIAL(debug) << GetName() << ": LookupAndUpdate failed: " << route.error().message()
                               << ", dropping packet";
      co_return ErrorCode{};
    }

    auto& mark = dynamic_cast<Mark&>(route->get());

    co_return co_await std::visit(
        Overload{[&](Mark::ToBeSelected) -> Omni::Fiber::Coroutine<ErrorCode> {
                   assert(false && "should not reach here");
                   std::unreachable();
                 },
                 [&](Mark::Bypass) -> Omni::Fiber::Coroutine<ErrorCode> {
                   assert(false && "should not reach here");
                   std::unreachable();
                 },
                 [&](Mark::Discard) -> Omni::Fiber::Coroutine<ErrorCode> {
                   BOOST_LOG_TRIVIAL(debug) << GetName() << ": Packet marked Discard, dropping";
                   co_return ErrorCode{};
                 },
                 [&](const std::weak_ptr<Session>& sessionWeak) -> Omni::Fiber::Coroutine<ErrorCode> {
                   if (auto session = sessionWeak.lock()) {
                     if (session->ChannelSide) {
                       auto result = co_await session->ChannelSide->PushOutgoing(std::move(packet));
                       if (!result.has_value()) {
                         BOOST_LOG_TRIVIAL(info) << GetName() << " PushOutgoing: ChannelSide is closed";
                       }
                     } else {
                       BOOST_LOG_TRIVIAL(info) << GetName() << ": ChannelSide is null, dropping packet";
                     }
                   } else {
                     BOOST_LOG_TRIVIAL(info) << GetName() << ": Session is expired, dropping packet";
                   }
                   co_return ErrorCode{};
                 }},
        mark.GetValue());
  }

protected:
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override { co_return ErrorCode{}; }
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override {
    _Pipe.GetConsumer().DiscardAndClose();
    co_return ErrorCode{};
  }

private:
  VpnClientMultiChannel& _Parent;
  ConnectionTracker& _ConnectionTracker;
  ConnectionTracker::Selector& _Selector;
  Omni::Fiber::Pipe<QueueEntry> _Pipe;
};

Omni::Fiber::Coroutine<ErrorCode> VpnClientMultiChannel::ChannelSideEndpoint::Write(Packet& packet, Cancel& c) {
  if (c.IsTriggered()) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }
  auto result = co_await _Parent._TunSide->PushIncoming(std::move(packet), _Session);
  if (!result.has_value()) {
    BOOST_LOG_TRIVIAL(info) << GetName() << " PushIncoming: TunSide is closed";
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }
  co_return ErrorCode{};
}

VpnClientMultiChannel::NoopSessionStateListener VpnClientMultiChannel::_NoopSessionStateListener;

VpnClientMultiChannel::VpnClientMultiChannel(boost::asio::any_io_executor executor, std::shared_ptr<Endpoint> tun,
                                             std::shared_ptr<UdpDynMux> udpDynMux,
                                             std::shared_ptr<ConnectionTracker> tracker,
                                             ConnectionTracker::Selector& selector,
                                             std::vector<std::shared_ptr<Filter>> filters,
                                             SessionStateListener& listener)
    : _Executor(executor), _Tun(tun), _UdpDynMux(udpDynMux), _ConnectionTracker(tracker), _Filters(filters),
      _StateListener(listener) {
  _TunSide = std::make_shared<TunSideEndpoint>(*this, *_ConnectionTracker, selector);
  _UdpDynMux->SetChannelNotification(*this);
}

VpnClientMultiChannel::~VpnClientMultiChannel() { assert(_Sessions.empty()); }

std::string VpnClientMultiChannel::GetName() const {
  return std::format("VpnClientMultiChannel:[{}]", _UdpDynMux->GetName());
}

Omni::Fiber::Coroutine<ErrorCode> VpnClientMultiChannel::DoStart() {
  auto err = co_await _ConnectionTracker->Start();
  if (err) {
    BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to start ConnectionTracker";
    co_return err;
  }

  err = co_await _TunSide->Start();
  if (err) {
    BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to start TunSideEndpoint";
    co_await _ConnectionTracker->Stop();
    co_return err;
  }

  _TunPipeline = std::make_shared<Pipeline>(_Tun, std::vector<std::shared_ptr<Filter>>{}, _TunSide);
  err = co_await _TunPipeline->Start();
  if (err) {
    BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to start TUN pipeline";
    co_await _TunSide->Stop();
    co_await _ConnectionTracker->Stop();
    co_return err;
  }
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<void> VpnClientMultiChannel::DoWork() {
  BOOST_LOG_TRIVIAL(info) << GetName() << " run loop started";

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

  BOOST_LOG_TRIVIAL(info) << GetName() << " run loop finished";
}

Omni::Fiber::Coroutine<ErrorCode> VpnClientMultiChannel::DoGracefulStop() {
  BOOST_LOG_TRIVIAL(info) << GetName() << " stopping";

  if (_TunPipeline) {
    co_await _TunPipeline->Stop();
    _TunPipeline.reset();
  }

  for (auto& [psk, session] : _Sessions) {
    _StateListener.get().OnSessionStopping(session);
    session->Running = false;
    if (session->SessionPipeline) {
      co_await session->SessionPipeline->Stop();
    }
    session->SessionPipeline.reset();
    if (session->ChannelSide) {
      co_await session->ChannelSide->Stop();
    }
    session->ChannelSide.reset();

    if (session->Channel) {
      co_await _UdpDynMux->RemoveChannel(psk);
    }
    _StateListener.get().OnSessionStopped(session);
  }
  _Sessions.clear();
  co_await _TunSide->Stop();
  co_await _ConnectionTracker->Stop();
  _ChannelCall.DiscardAndClose();

  BOOST_LOG_TRIVIAL(info) << GetName() << " stopped";
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<std::shared_ptr<VpnClientMultiChannel::Session>>
VpnClientMultiChannel::RegisterChannel(const UdpDynMux::PskType& psk, std::shared_ptr<ResolverEndpoint> resolver) {
  auto [it, inserted] = _Sessions.emplace(psk, nullptr);
  if (inserted) {
    it->second = std::make_shared<Session>();
  } else {
    co_return it->second;
  }
  _StateListener.get().OnSessionStarting(it->second);
  it->second->Channel = co_await _UdpDynMux->CreateChannel(psk, resolver);
  if (!it->second->Channel) {
    _StateListener.get().OnSessionFailed(it->second, "Failed to create channel");
  }
  co_return it->second;
}

Omni::Fiber::Coroutine<void>
VpnClientMultiChannel::UnregisterChannel(std::shared_ptr<VpnClientMultiChannel::Session> session) {
  _StateListener.get().OnSessionStopping(session);
  session->Running = false;
  if (session->Channel) {
    auto psk = session->Channel->GetPsk();
    co_await _UdpDynMux->RemoveChannel(psk);
    _StateListener.get().OnSessionStopped(session);
    _Sessions.erase(psk);
  } else {
    _StateListener.get().OnSessionStopped(session);
    std::erase_if(_Sessions, [&](const auto& pair) { return pair.second == session; });
  }
}

Omni::Fiber::Coroutine<void> VpnClientMultiChannel::OnChannelEstablished(std::shared_ptr<UdpDynMux::Channel> channel) {
  BOOST_LOG_TRIVIAL(info) << GetName() << ": on channel established: " << channel->GetName();
  if (_State != State::kRunning) {
    co_return;
  }
  auto result =
      co_await _ChannelCall.Call([this, channel = std::move(channel)]() mutable -> Omni::Fiber::Coroutine<void> {
        if (_State != State::kRunning) {
          co_return;
        }
        auto it = _Sessions.find(channel->GetPsk());
        if (it != _Sessions.end()) {
          auto session = it->second;
          session->Channel = channel;
          session->Running = true;

          if (!session->SessionPipeline) {
            auto channelSide = std::make_shared<ChannelSideEndpoint>(*this, session);
            auto errChannel = co_await channelSide->Start();
            if (errChannel) {
              BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to start channel side for " << channel->GetName();
              co_await channelSide->Stop();
              _StateListener.get().OnSessionFailed(session, "Failed to start channel side: " + errChannel.message());
              co_return;
            }
            auto pipeline = std::make_shared<Pipeline>(channel, _Filters, channelSide);
            auto err = co_await pipeline->Start();
            if (err) {
              BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to start channel pipeline for " << channel->GetName();
              co_await pipeline->Stop();
              _StateListener.get().OnSessionFailed(session, "Failed to start channel pipeline: " + err.message());
              co_return;
            }
            session->ChannelSide = std::move(channelSide);
            session->SessionPipeline = std::move(pipeline);
          }
          _StateListener.get().OnSessionRunning(session);
        } else {
          BOOST_LOG_TRIVIAL(warning) << GetName() << ": Established channel for unregistered PSK, ignoring";
        }
      });
  if (!result.has_value()) {
    BOOST_LOG_TRIVIAL(error) << GetName() << ": RPC call failed for OnChannelEstablished";
  }
}

Omni::Fiber::Coroutine<void> VpnClientMultiChannel::OnChannelClosed(std::shared_ptr<UdpDynMux::Channel> channel) {
  BOOST_LOG_TRIVIAL(info) << GetName() << ": on channel closed: " << channel->GetName();
  if (_State != State::kRunning) {
    co_return;
  }
  auto result =
      co_await _ChannelCall.Call([this, channel = std::move(channel)]() mutable -> Omni::Fiber::Coroutine<void> {
        if (_State != State::kRunning) {
          co_return;
        }
        auto it = _Sessions.find(channel->GetPsk());
        if (it != _Sessions.end()) {
          auto session = it->second;
          session->Running = false;
          if (session->SessionPipeline) {
            co_await session->SessionPipeline->Stop();
          }
          session->SessionPipeline.reset();
          if (session->ChannelSide) {
            co_await session->ChannelSide->Stop();
          }
          session->ChannelSide.reset();
          _StateListener.get().OnSessionStopped(session);
        }
      });
  if (!result.has_value()) {
    BOOST_LOG_TRIVIAL(error) << GetName() << ": RPC call failed for OnChannelClosed";
  }
}

Omni::Fiber::Coroutine<ErrorCode> VpnClientMultiChannel::MigrateTun(std::shared_ptr<Endpoint> newTun) {
  auto ec = co_await _ChannelCall.Call([this, newTun]() -> Omni::Fiber::Coroutine<ErrorCode> {
    BOOST_LOG_TRIVIAL(info) << GetName() << ": migrating TUN endpoint";
    if (_TunPipeline) {
      co_await _TunPipeline->Stop();
      _TunPipeline.reset();
    }
    _Tun = newTun;
    _TunPipeline = std::make_shared<Pipeline>(_Tun, std::vector<std::shared_ptr<Filter>>{}, _TunSide);
    co_return co_await _TunPipeline->Start();
  });
  if (ec.has_value()) {
    if (ec.value()) {
      BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to migrate TUN endpoint: " << ec.value().message();
    }
    co_return ec.value();
  } else {
    BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to migrate TUN endpoint: _ChannelCall closed";
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }
}

std::optional<VpnTrafficStats>
VpnClientMultiChannel::GetStats(std::shared_ptr<VpnClientMultiChannel::Session> session) const {
  if (session->SessionPipeline) {
    int64_t rttMs = -1;
    if (session->Channel) {
      rttMs = session->Channel->GetRoundTripTime().count();
    }
    return VpnTrafficStats{session->SessionPipeline->GetTrafficStats(), rttMs};
  }
  return std::nullopt;
}

} // namespace gh
