#include "VpnClientMultiChannel.hpp"

#include <array>
#include <chrono>
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
#include "GetCurrentFiber.hpp"
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

class VpnClientMultiChannel::TunSideEndpoint : public EndpointSkipStart<Endpoint> {
public:
  struct QueueEntry {
    Packet Pkt;
    std::shared_ptr<UdpDynMux::Channel> Channel;
  };

  explicit TunSideEndpoint(VpnClientMultiChannel& parent) : _Parent(parent) {}
  ~TunSideEndpoint() override = default;

  std::string GetName() const override { return "VpnClientMultiChannel:TunSide"; }

  void PushIncoming(Packet p, std::shared_ptr<UdpDynMux::Channel> channel) {
    _Queue.Push(QueueEntry{std::move(p), std::move(channel)});
  }

  void RemoveChannel(const std::shared_ptr<UdpDynMux::Channel>& channel) { _ConnectionTracker.RemoveChannel(channel); }

  void PruneConnections() { _ConnectionTracker.Prune(); }

  void ClearConnections() { _ConnectionTracker.Clear(); }

  void SetConntrackTimeout(std::chrono::seconds timeout) { _ConnectionTracker.SetTimeout(timeout); }

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
    _ConnectionTracker.Update(p, entry.Channel, ConnectionDirection::kInput);

    co_return ErrorCode{};
  }

  Omni::Fiber::Coroutine<ErrorCode> Write(Packet& p, Cancel& c) override {
    if (c.IsTriggered()) {
      co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
    }

    auto channel = std::dynamic_pointer_cast<UdpDynMux::Channel>(_ConnectionTracker.Lookup(
        p, ConnectionDirection::kOutput,
        [this](const std::shared_ptr<Endpoint>& chan) {
          auto udpChan = std::dynamic_pointer_cast<UdpDynMux::Channel>(chan);
          return udpChan && _Parent._Sessions.contains(udpChan);
        },
        [this](const boost::asio::ip::address_v6& src, const boost::asio::ip::address_v6& dst, uint16_t srcPort,
               uint16_t dstPort, uint8_t protocol) -> std::shared_ptr<Endpoint> {
          if (_Parent._Selector) {
            return _Parent._Selector(src, dst, srcPort, dstPort, protocol);
          }
          return nullptr;
        }));

    if (!channel) {
      BOOST_LOG_TRIVIAL(debug) << _Parent.GetName() << ": No channel found or selected, dropping packet";
      co_return ErrorCode{};
    }

    auto sessionIt = _Parent._Sessions.find(channel);
    if (sessionIt != _Parent._Sessions.end()) {
      sessionIt->second.ChannelSide->PushOutgoing(std::move(p));
    } else {
      BOOST_LOG_TRIVIAL(debug) << _Parent.GetName() << ": Session for channel not found, dropping packet";
    }

    co_return ErrorCode{};
  }

protected:
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override { co_return ErrorCode{}; }
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override { co_return ErrorCode{}; }

private:
  VpnClientMultiChannel& _Parent;
  ConnectionTracker _ConnectionTracker;
  Omni::Fiber::EventQueue<QueueEntry> _Queue;
};

Omni::Fiber::Coroutine<ErrorCode> VpnClientMultiChannel::ChannelSideEndpoint::Write(Packet& p, Cancel& c) {
  if (c.IsTriggered()) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }
  _Parent._TunSide->PushIncoming(std::move(p), _Channel);
  co_return ErrorCode{};
}

VpnClientMultiChannel::VpnClientMultiChannel(boost::asio::io_context& ioContext, std::shared_ptr<Endpoint> tun,
                                             std::shared_ptr<UdpDynMux> udpDynMux, PeerSelector selector,
                                             std::vector<std::shared_ptr<Filter>> filters)
    : _IoContext(ioContext), _Tun(tun), _UdpDynMux(udpDynMux), _Selector(selector), _Filters(filters) {
  _TunSide = std::make_shared<TunSideEndpoint>(*this);
  if (_UdpDynMux) {
    _UdpDynMux->SetChannelNotification(*this);
  }
}

VpnClientMultiChannel::~VpnClientMultiChannel() { assert(_Sessions.empty()); }

void VpnClientMultiChannel::SetConntrackTimeoutForTesting(std::chrono::seconds timeout) {
  _TunSide->SetConntrackTimeout(timeout);
}

std::string VpnClientMultiChannel::GetName() const {
  return std::format("VpnClientMultiChannel:[{}]", _UdpDynMux ? _UdpDynMux->GetName() : "null");
}

Omni::Fiber::Coroutine<ErrorCode> VpnClientMultiChannel::DoStart() {
  _TunPipeline = std::make_shared<Pipeline>(_Tun, std::vector<std::shared_ptr<Filter>>{}, _TunSide);
  auto err = co_await _TunPipeline->Start();
  if (err) {
    BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to start TUN pipeline";
    co_return err;
  }
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<void> VpnClientMultiChannel::DoWork() {
  BOOST_LOG_TRIVIAL(info) << GetName() << " run loop started";

  auto& currentFiber = co_await Omni::Fiber::GetCurrentFiber();
  auto pruneFiber = currentFiber.Spawn(GetName() + ":PruneLoop", [this]() -> Omni::Fiber::Coroutine<void> {
    co_await PruneLoop();
    co_return;
  });

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

  co_await currentFiber.Join(pruneFiber);
  BOOST_LOG_TRIVIAL(info) << GetName() << " run loop finished";
}

Omni::Fiber::Coroutine<ErrorCode> VpnClientMultiChannel::DoGracefulStop() {
  BOOST_LOG_TRIVIAL(info) << GetName() << " stopping";

  if (_TunPipeline) {
    co_await _TunPipeline->Stop();
    _TunPipeline.reset();
  }

  for (auto& [channel, session] : _Sessions) {
    co_await session.Pipeline->Stop();
  }
  _Sessions.clear();
  _TunSide->ClearConnections();

  co_await _ChannelCall.Close();

  BOOST_LOG_TRIVIAL(info) << GetName() << " stopped";
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<void> VpnClientMultiChannel::OnChannelEstablished(std::shared_ptr<UdpDynMux::Channel> channel) {
  BOOST_LOG_TRIVIAL(info) << GetName() << ": on channel established: " << channel->GetName();
  co_await _ChannelCall.Call([this, channel = std::move(channel)]() mutable -> Omni::Fiber::Coroutine<void> {
    if (_State != State::kRunning) {
      co_return;
    }
    auto channelSide = std::make_shared<ChannelSideEndpoint>(*this, channel);
    auto pipeline = std::make_shared<Pipeline>(channel, _Filters, channelSide);
    auto err = co_await pipeline->Start();
    if (err) {
      BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to start channel pipeline for " << channel->GetName();
      co_await pipeline->Stop();
      co_return;
    }

    _Sessions.emplace(std::move(channel), Session{std::move(channelSide), std::move(pipeline)});
  });
}

Omni::Fiber::Coroutine<void> VpnClientMultiChannel::OnChannelClosed(std::shared_ptr<UdpDynMux::Channel> channel) {
  BOOST_LOG_TRIVIAL(info) << GetName() << ": on channel closed: " << channel->GetName();
  co_await _ChannelCall.Call([this, channel = std::move(channel)]() mutable -> Omni::Fiber::Coroutine<void> {
    auto it = _Sessions.find(channel);
    if (it != _Sessions.end()) {
      co_await it->second.Pipeline->Stop();
      _Sessions.erase(it);
    }
    _TunSide->RemoveChannel(channel);
  });
}

Omni::Fiber::Coroutine<void> VpnClientMultiChannel::PruneLoop() {
  boost::asio::steady_timer pruneTimer(_IoContext.get_executor());
  while (_State == State::kRunning && !_Service.value()._Stop.IsTriggered()) {
    pruneTimer.expires_after(std::chrono::seconds(5));
    auto [err] = co_await pruneTimer.async_wait(_Service.value()._Stop.AsioSlot()());
    if (err) {
      break;
    }

    _TunSide->PruneConnections();
  }
}

} // namespace gh
