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
#include "EventQueue.hpp"
#include "Packet.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"
#include "ServiceBase.hpp"

namespace gh {

class VpnClientMultiChannel::ChannelSideEndpoint : public Endpoint {
public:
  ChannelSideEndpoint(VpnClientMultiChannel& parent, std::shared_ptr<UdpDynMux::Channel> channel)
      : _Parent(parent), _Channel(channel) {}
  ~ChannelSideEndpoint() override = default;

  std::string GetName() const override {
    return std::format("VpnClientMultiChannel:ChannelSide:[{}]", _Channel->GetName());
  }

  Omni::Fiber::Coroutine<ErrorCode> Read(Packet& p, Cancel& c) override {
    while (_Queue.IsEmpty()) {
      if (c.IsTriggered()) {
        co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
      }
      auto [cancelFired, queueFired] = co_await Omni::Fiber::Select(
          Omni::Fiber::SelectPair(c.GetFiberCancelEvent(), [] {}), Omni::Fiber::SelectPair(_Queue, [] {}));
      if (cancelFired) {
        co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
      }
    }
    p = _Queue.PopFront();
    co_return ErrorCode{};
  }

  Omni::Fiber::Coroutine<ErrorCode> Write(Packet& p, Cancel& c) override;

  void PushOutgoing(Packet p) { _Queue.Push(std::move(p)); }

protected:
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override { co_return ErrorCode{}; }
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override { co_return ErrorCode{}; }

private:
  VpnClientMultiChannel& _Parent;
  std::shared_ptr<UdpDynMux::Channel> _Channel;
  Omni::Fiber::EventQueue<Packet> _Queue;
};

class VpnClientMultiChannel::TunSideEndpoint : public Endpoint {
public:
  struct QueueEntry {
    Packet Pkt;
    std::shared_ptr<UdpDynMux::Channel> Channel;
  };

  explicit TunSideEndpoint(VpnClientMultiChannel& parent)
      : _Parent(parent), _ConnectionTracker(std::make_shared<ConnectionTracker>(parent._Executor, parent._Selector)) {}
  ~TunSideEndpoint() override = default;

  std::string GetName() const override { return "VpnClientMultiChannel:TunSide"; }

  Omni::Fiber::Coroutine<ErrorCode> Start() override { co_return co_await _ConnectionTracker->Start(); }

  Omni::Fiber::Coroutine<ErrorCode> Stop() override {
    co_await _ConnectionTracker->Stop();
    co_await _ConnectionTracker->WaitService();
    co_return ErrorCode{};
  }

  void PushIncoming(Packet p, std::shared_ptr<UdpDynMux::Channel> channel) {
    _Queue.Push(QueueEntry{std::move(p), std::move(channel)});
  }

  void RemoveMark(const ConnectionMark& mark) { _ConnectionTracker->RemoveMark(mark); }

  Omni::Fiber::Coroutine<ErrorCode> Read(Packet& p, Cancel& c) override {
    while (_Queue.IsEmpty()) {
      if (c.IsTriggered()) {
        co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
      }
      auto [cancelFired, queueFired] = co_await Omni::Fiber::Select(
          Omni::Fiber::SelectPair(c.GetFiberCancelEvent(), [] {}), Omni::Fiber::SelectPair(_Queue, [] {}));
      if (cancelFired) {
        co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
      }
    }

    auto entry = _Queue.PopFront();
    p = std::move(entry.Pkt);

    // Perform connection tracking for incoming traffic
    auto it = _Parent._Sessions.find(entry.Channel->GetPsk());
    if (it != _Parent._Sessions.end()) {
      _ConnectionTracker->Update(p, it->second, ConnectionDirection::kInput);
    }

    co_return ErrorCode{};
  }

  Omni::Fiber::Coroutine<ErrorCode> Write(Packet& p, Cancel& c) override {
    if (c.IsTriggered()) {
      co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
    }

    auto mark = _ConnectionTracker->Lookup(p, ConnectionDirection::kOutput, [this](ConnectionMark& mark) -> bool {
      return dynamic_cast<Session&>(mark).Running;
    });

    if (!mark.has_value()) {
      BOOST_LOG_TRIVIAL(debug) << _Parent.GetName() << ": No channel found or selected, dropping packet";
      co_return ErrorCode{};
    }

    auto& session = dynamic_cast<Session&>(mark.value().get());
    if (session.ChannelSide) {
      session.ChannelSide->PushOutgoing(std::move(p));
    } else {
      BOOST_LOG_TRIVIAL(debug) << _Parent.GetName() << ": ChannelSide is null, dropping packet";
    }

    co_return ErrorCode{};
  }

protected:
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override { co_return ErrorCode{}; }
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override { co_return ErrorCode{}; }

private:
  VpnClientMultiChannel& _Parent;
  std::shared_ptr<ConnectionTracker> _ConnectionTracker;
  Omni::Fiber::EventQueue<QueueEntry> _Queue;
};

Omni::Fiber::Coroutine<ErrorCode> VpnClientMultiChannel::ChannelSideEndpoint::Write(Packet& p, Cancel& c) {
  if (c.IsTriggered()) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }
  _Parent._TunSide->PushIncoming(std::move(p), _Channel);
  co_return ErrorCode{};
}

VpnClientMultiChannel::VpnClientMultiChannel(boost::asio::any_io_executor executor, std::shared_ptr<Endpoint> tun,
                                             std::shared_ptr<UdpDynMux> udpDynMux,
                                             ConnectionTracker::SelectorType selector,
                                             std::vector<std::shared_ptr<Filter>> filters)
    : _Executor(executor), _Tun(tun), _UdpDynMux(udpDynMux), _Selector(selector), _Filters(filters) {
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
    if (session.SessionPipeline) {
      co_await session.SessionPipeline->Stop();
    }
  }
  _Sessions.clear();
  co_await _TunSide->Stop();
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
  it->second.Channel = co_await _UdpDynMux->CreateChannel(psk, resolver);
  co_return std::ref(it->second);
}

Omni::Fiber::Coroutine<void> VpnClientMultiChannel::UnregisterChannel(Session& session) {
  session.Running = false;
  auto psk = session.Channel->GetPsk();
  co_await _UdpDynMux->RemoveChannel(psk);
  _Sessions.erase(psk);
}

Omni::Fiber::Coroutine<void> VpnClientMultiChannel::OnChannelEstablished(std::shared_ptr<UdpDynMux::Channel> channel) {
  BOOST_LOG_TRIVIAL(info) << GetName() << ": on channel established: " << channel->GetName();
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
        auto channelSide = std::make_shared<ChannelSideEndpoint>(*this, channel);
        auto pipeline = std::make_shared<Pipeline>(channel, _Filters, channelSide);
        auto err = co_await pipeline->Start();
        if (err) {
          BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to start channel pipeline for " << channel->GetName();
          co_await pipeline->Stop();
          co_return;
        }
        session.ChannelSide = std::move(channelSide);
        session.SessionPipeline = std::move(pipeline);
      }
    } else {
      BOOST_LOG_TRIVIAL(warning) << GetName() << ": Established channel for unregistered PSK, ignoring";
    }
  });
}

Omni::Fiber::Coroutine<void> VpnClientMultiChannel::OnChannelClosed(std::shared_ptr<UdpDynMux::Channel> channel) {
  BOOST_LOG_TRIVIAL(info) << GetName() << ": on channel closed: " << channel->GetName();
  co_await _ChannelCall.Call([this, channel = std::move(channel)]() mutable -> Omni::Fiber::Coroutine<void> {
    auto it = _Sessions.find(channel->GetPsk());
    if (it != _Sessions.end()) {
      it->second.Running = false;
      if (it->second.SessionPipeline) {
        co_await it->second.SessionPipeline->Stop();
      }
      it->second.SessionPipeline.reset();
      it->second.ChannelSide.reset();
      _TunSide->RemoveMark(it->second);
    }
  });
}

std::pair<uint64_t, uint64_t> VpnClientMultiChannel::GetStats(Session& session) const {
  return {session.Channel->GetTxBytes(), session.Channel->GetRxBytes()};
}

} // namespace gh
