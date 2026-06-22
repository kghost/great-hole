#include "VpnClientMultiChannel.hpp"

#include <array>
#include <boost/asio/any_io_executor.hpp>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <boost/asio.hpp>

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

namespace gh {

class VpnClientMultiChannel::ChannelSideEndpoint : public Endpoint {
public:
  ChannelSideEndpoint(VpnClientMultiChannel& parent, Session& session) : _Parent(parent), _Session(session) {}
  ~ChannelSideEndpoint() override = default;

  std::string GetName() const override {
    return std::format("VpnClientMultiChannel:ChannelSide:[{}]", _Session.Channel->GetName());
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

  Omni::Fiber::Coroutine<ErrorCode> Write(Packet& p, Cancel& c) override;

  Omni::Fiber::Coroutine<void> PushOutgoing(Packet p) {
    co_await _Pipe.GetProducer().Put(std::move(p));
    co_return;
  }

protected:
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override { co_return ErrorCode{}; }
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override {
    co_await _Pipe.GetProducer().Close();
    co_return ErrorCode{};
  }

private:
  VpnClientMultiChannel& _Parent;
  Session& _Session;
  Omni::Fiber::Pipe<Packet> _Pipe;
};

class VpnClientMultiChannel::TunSideEndpoint : public Endpoint {
public:
  struct QueueEntry {
    Packet Pkt;
    Session& PacketSession;
  };

  explicit TunSideEndpoint(VpnClientMultiChannel& parent)
      : _Parent(parent), _ConnectionTracker(std::make_shared<ConnectionTracker>(parent._Executor, parent._Selector)) {}
  ~TunSideEndpoint() override = default;

  std::string GetName() const override { return "VpnClientMultiChannel:TunSide"; }

  Omni::Fiber::Coroutine<void> PushIncoming(Packet p, Session& session) {
    co_await _Pipe.GetProducer().Put(QueueEntry{std::move(p), session});
    co_return;
  }

  void RemoveMark(const ConnectionMark& mark) { _ConnectionTracker->RemoveMark(mark); }

  Omni::Fiber::Coroutine<ErrorCode> Read(Packet& p, Cancel& c) override {
    if (c.IsTriggered()) {
      co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
    }
    auto [cancelFired, queueFired] = co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(c.GetFiberCancelEvent(), [] {}),
        Omni::Fiber::SelectPair(_Pipe.GetConsumer(), [&p, this](auto&& entry) -> ErrorCode {
          if (entry.has_value()) {
            p = std::move(entry.value().Pkt);
            // Perform connection tracking for incoming traffic
            _ConnectionTracker->LookupAndUpdate<ConnectionDirection::kInput>(p, entry.value().PacketSession);
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

  Omni::Fiber::Coroutine<ErrorCode> Write(Packet& p, Cancel& c) override {
    if (c.IsTriggered()) {
      co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
    }

    auto mark = _ConnectionTracker->LookupAndUpdate<ConnectionDirection::kOutput>(
        p, std::nullopt, [this](ConnectionMark& mark) -> bool { return dynamic_cast<Session&>(mark).Running; });

    if (!mark.has_value()) {
      BOOST_LOG_TRIVIAL(debug) << _Parent.GetName() << ": No channel found or selected, dropping packet";
      co_return ErrorCode{};
    }

    auto& session = dynamic_cast<Session&>(mark.value().get());
    if (session.ChannelSide) {
      co_await session.ChannelSide->PushOutgoing(std::move(p));
    } else {
      BOOST_LOG_TRIVIAL(debug) << _Parent.GetName() << ": ChannelSide is null, dropping packet";
    }

    co_return ErrorCode{};
  }

protected:
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override { co_return co_await _ConnectionTracker->Start(); }
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override {
    co_await _Pipe.GetProducer().Close();

    co_await _ConnectionTracker->Stop();
    co_await _ConnectionTracker->WaitService();

    co_return ErrorCode{};
  }

private:
  VpnClientMultiChannel& _Parent;
  std::shared_ptr<ConnectionTracker> _ConnectionTracker;
  Omni::Fiber::Pipe<QueueEntry> _Pipe;
};

Omni::Fiber::Coroutine<ErrorCode> VpnClientMultiChannel::ChannelSideEndpoint::Write(Packet& p, Cancel& c) {
  if (c.IsTriggered()) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }
  co_await _Parent._TunSide->PushIncoming(std::move(p), _Session);
  co_return ErrorCode{};
}

VpnClientMultiChannel::NoopSessionStateListener VpnClientMultiChannel::_NoopSessionStateListener;

VpnClientMultiChannel::VpnClientMultiChannel(boost::asio::any_io_executor executor, std::shared_ptr<Endpoint> tun,
                                             std::shared_ptr<UdpDynMux> udpDynMux,
                                             ConnectionTracker::Selector& selector,
                                             std::vector<std::shared_ptr<Filter>> filters,
                                             SessionStateListener& listener)
    : _Executor(executor), _Tun(tun), _UdpDynMux(udpDynMux), _Selector(selector), _Filters(filters),
      _StateListener(listener) {
  _TunSide = std::make_shared<TunSideEndpoint>(*this);
  _UdpDynMux->SetChannelNotification(*this);
}

VpnClientMultiChannel::~VpnClientMultiChannel() { assert(_Sessions.empty()); }

std::string VpnClientMultiChannel::GetName() const {
  return std::format("VpnClientMultiChannel:[{}]", _UdpDynMux->GetName());
}

Omni::Fiber::Coroutine<ErrorCode> VpnClientMultiChannel::DoStart() {
  auto err = co_await _TunSide->Start();
  if (err) {
    BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to start TunSideEndpoint";
    co_return err;
  }

  _TunPipeline = std::make_shared<Pipeline>(_Tun, std::vector<std::shared_ptr<Filter>>{}, _TunSide);
  err = co_await _TunPipeline->Start();
  if (err) {
    BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to start TUN pipeline";
    co_await _TunSide->Stop();
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
    session.Running = false;
    if (session.SessionPipeline) {
      co_await session.SessionPipeline->Stop();
    }
    session.SessionPipeline.reset();
    if (session.ChannelSide) {
      co_await session.ChannelSide->Stop();
      co_await session.ChannelSide->WaitService();
    }
    session.ChannelSide.reset();

    if (session.Channel) {
      co_await _UdpDynMux->RemoveChannel(psk);
    }
    _StateListener.get().OnSessionStopped(session);
  }
  _Sessions.clear();
  co_await _TunSide->Stop();
  co_await _TunSide->WaitService();
  co_await _ChannelCall.Close();

  BOOST_LOG_TRIVIAL(info) << GetName() << " stopped";
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<std::reference_wrapper<VpnClientMultiChannel::Session>>
VpnClientMultiChannel::RegisterChannel(const UdpDynMux::PskType& psk, std::shared_ptr<ResolverEndpoint> resolver) {
  auto [it, inserted] = _Sessions.emplace(psk, Session{});
  if (!inserted) {
    co_return std::ref(it->second);
  }
  _StateListener.get().OnSessionStarting(it->second);
  it->second.Channel = co_await _UdpDynMux->CreateChannel(psk, resolver);
  if (!it->second.Channel) {
    _StateListener.get().OnSessionFailed(it->second, "Failed to create channel");
  }
  co_return std::ref(it->second);
}

Omni::Fiber::Coroutine<void> VpnClientMultiChannel::UnregisterChannel(Session& session) {
  _StateListener.get().OnSessionStopping(session);
  session.Running = false;
  if (session.Channel) {
    auto psk = session.Channel->GetPsk();
    co_await _UdpDynMux->RemoveChannel(psk);
    _TunSide->RemoveMark(session);
    _StateListener.get().OnSessionStopped(session);
    _Sessions.erase(psk);
  } else {
    _StateListener.get().OnSessionStopped(session);
    std::erase_if(_Sessions, [&](const auto& pair) { return &pair.second == &session; });
  }
}

Omni::Fiber::Coroutine<void> VpnClientMultiChannel::OnChannelEstablished(std::shared_ptr<UdpDynMux::Channel> channel) {
  BOOST_LOG_TRIVIAL(info) << GetName() << ": on channel established: " << channel->GetName();
  if (_State != State::kRunning) {
    co_return;
  }
  co_await _ChannelCall.Call([this, channel = std::move(channel)]() mutable -> Omni::Fiber::Coroutine<void> {
    if (_State != State::kRunning) {
      co_return;
    }
    auto it = _Sessions.find(channel->GetPsk());
    if (it != _Sessions.end()) {
      auto& session = it->second;
      session.Channel = channel;
      session.Running = true;

      if (!session.SessionPipeline) {
        auto channelSide = std::make_shared<ChannelSideEndpoint>(*this, session);
        auto errChannel = co_await channelSide->Start();
        if (errChannel) {
          BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to start channel side for " << channel->GetName();
          co_await channelSide->Stop();
          co_await channelSide->WaitService();
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
        session.ChannelSide = std::move(channelSide);
        session.SessionPipeline = std::move(pipeline);
      }
      _StateListener.get().OnSessionRunning(session);
    } else {
      BOOST_LOG_TRIVIAL(warning) << GetName() << ": Established channel for unregistered PSK, ignoring";
    }
  });
}

Omni::Fiber::Coroutine<void> VpnClientMultiChannel::OnChannelClosed(std::shared_ptr<UdpDynMux::Channel> channel) {
  BOOST_LOG_TRIVIAL(info) << GetName() << ": on channel closed: " << channel->GetName();
  if (_State != State::kRunning) {
    co_return;
  }
  co_await _ChannelCall.Call([this, channel = std::move(channel)]() mutable -> Omni::Fiber::Coroutine<void> {
    if (_State != State::kRunning) {
      co_return;
    }
    auto it = _Sessions.find(channel->GetPsk());
    if (it != _Sessions.end()) {
      auto& session = it->second;
      session.Running = false;
      if (session.SessionPipeline) {
        co_await session.SessionPipeline->Stop();
      }
      session.SessionPipeline.reset();
      if (session.ChannelSide) {
        co_await session.ChannelSide->Stop();
        co_await session.ChannelSide->WaitService();
      }
      session.ChannelSide.reset();
      _StateListener.get().OnSessionStopped(session);
    }
  });
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

std::optional<TrafficStats> VpnClientMultiChannel::GetStats(Session& session) const {
  if (session.SessionPipeline) {
    return session.SessionPipeline->GetTrafficStats();
  }
  return std::nullopt;
}

} // namespace gh
