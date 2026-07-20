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
#include "Interface.hpp"
#include "Packet.hpp"
#include "Pipe.hpp"
#include "ResolverHelper.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"
#include "ServiceBase.hpp"
#include "Utils/Overload.hpp"

namespace gh {

class SessionSelector : public ConnectionTracker::Selector {
public:
  explicit SessionSelector(std::weak_ptr<VpnClientMultiChannelSession> session) : _Session(std::move(session)) {}
  ~SessionSelector() override = default;

  SessionSelector(const SessionSelector&) = delete;
  auto operator=(const SessionSelector&) -> SessionSelector& = delete;
  SessionSelector(SessionSelector&&) = delete;
  auto operator=(SessionSelector&&) -> SessionSelector& = delete;

  auto SelectConnectionMark(const ConnectionTracker::ConnectionKey& /*unused*/)
      -> std::shared_ptr<ConnectionMark> override {
    return std::make_unique<VpnClientMultiChannel::Mark>(_Session);
  }

private:
  std::weak_ptr<VpnClientMultiChannelSession> _Session;
};

auto VpnClientMultiChannel::Mark::GetDescription() const -> std::string {
  return std::visit(Overload{[](const ToBeSelected&) -> std::string { return "ToBeSelected"; },
                             [](const Bypass&) -> std::string { return "Bypass"; },
                             [](const Discard&) -> std::string { return "Discard"; },
                             [](const Deferred&) -> std::string { return "Deferred"; },
                             [](const Interface::VpnEndpoint& endpoint) -> std::string {
                               if (auto session = endpoint.lock()) {
                                 return session->GetDescription();
                               }
                               return "Expired Session";
                             }},
                    _Value);
}

auto VpnClientMultiChannel::Mark::Validate() const -> bool {
  return std::visit(Overload{[](const ToBeSelected&) -> bool { return false; },
                             [](const Bypass&) -> bool { return true; }, [](const Discard&) -> bool { return true; },
                             [](const Deferred&) -> bool { return true; },
                             [](const Interface::VpnEndpoint& endpoint) -> bool {
                               if (auto session = endpoint.lock()) {
                                 return session->Running;
                               }
                               return false;
                             }},
                    _Value);
}

class VpnClientMultiChannel::ChannelSideEndpoint : public Endpoint {
public:
  ChannelSideEndpoint(VpnClientMultiChannel& parent, std::shared_ptr<VpnClientMultiChannelSession> session)
      : _Parent(parent), _Session(std::move(session)) {}
  ~ChannelSideEndpoint() override = default;

  ChannelSideEndpoint(const ChannelSideEndpoint&) = delete;
  auto operator=(const ChannelSideEndpoint&) -> ChannelSideEndpoint& = delete;
  ChannelSideEndpoint(ChannelSideEndpoint&&) = delete;
  auto operator=(ChannelSideEndpoint&&) -> ChannelSideEndpoint& = delete;

  auto GetName() const -> std::string override {
    return std::format("VpnClientMultiChannel:ChannelSide:[{}]", _Session->Channel->GetName());
  }

  auto Read(Packet& packet, Cancel& cancel) -> Omni::Fiber::Coroutine<ErrorCode> override {
    if (cancel.IsTriggered()) {
      co_return Error(AppErrorCategory::kOperationAborted);
    }
    auto [cancelFired, queueFired] =
        co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(cancel.GetFiberCancelEvent(), [] -> void {}),
                                     Omni::Fiber::SelectPair(_Pipe.GetConsumer(), [&packet](auto pkt) -> ErrorCode {
                                       if (pkt.has_value()) {
                                         packet = std::move(pkt.value());
                                         return ErrorCode{};
                                       } else {
                                         return Error(AppErrorCategory::kEndOfStream);
                                       }
                                     }));
    if (queueFired.has_value()) {
      co_return queueFired.value();
    }
    if (cancelFired) {
      co_return Error(AppErrorCategory::kOperationAborted);
    }
    assert(false && "should not reach here");
    co_return ErrorCode{};
  }

  auto Write(Packet& packet, Cancel& cancel) -> Omni::Fiber::Coroutine<ErrorCode> override;

  auto PushOutgoing(Packet packet) -> Omni::Fiber::Coroutine<std::expected<void, Omni::Fiber::PipeClosed>> {
    co_return co_await _Pipe.GetProducer().Put(std::move(packet));
  }

protected:
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override { co_return ErrorCode{}; }
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override {
    _Pipe.GetConsumer().DiscardAndClose();
    co_return ErrorCode{};
  }

private:
  VpnClientMultiChannel& _Parent;
  std::shared_ptr<VpnClientMultiChannelSession> _Session;
  Omni::Fiber::Pipe<Packet> _Pipe;
};

class VpnClientMultiChannel::TunSideEndpoint : public Endpoint {
public:
  struct QueueEntry {
    Packet PacketData;
    std::weak_ptr<VpnClientMultiChannelSession> PacketSession;
  };

  explicit TunSideEndpoint(VpnClientMultiChannel& parent, ConnectionTracker& tracker,
                           ConnectionTracker::Selector& selector)
      : _Parent(parent), _ConnectionTracker(tracker), _Selector(selector) {}
  ~TunSideEndpoint() override = default;

  TunSideEndpoint(const TunSideEndpoint&) = delete;
  auto operator=(const TunSideEndpoint&) -> TunSideEndpoint& = delete;
  TunSideEndpoint(TunSideEndpoint&&) = delete;
  auto operator=(TunSideEndpoint&&) -> TunSideEndpoint& = delete;

  auto GetName() const -> std::string override {
    return std::format("VpnClientMultiChannel:TunSide:{}", _Parent.GetName());
  }

  auto PushIncoming(Packet packet, std::weak_ptr<VpnClientMultiChannelSession> session)
      -> Omni::Fiber::Coroutine<std::expected<void, Omni::Fiber::PipeClosed>> {
    co_return co_await _Pipe.GetProducer().Put(QueueEntry{.PacketData = std::move(packet), .PacketSession = session});
  }

  auto Read(Packet& packet, Cancel& cancel) -> Omni::Fiber::Coroutine<ErrorCode> override {
    if (cancel.IsTriggered()) {
      co_return Error(AppErrorCategory::kOperationAborted);
    }
    auto [cancelFired, queueFired] = co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(cancel.GetFiberCancelEvent(), [] -> void {}),
        Omni::Fiber::SelectPair(_Pipe.GetConsumer(), [&packet, this](auto&& entry) -> ErrorCode {
          if (entry.has_value()) {
            packet = std::move(entry.value().PacketData);
            SessionSelector sessionSelector(entry.value().PacketSession);
            auto res = _ConnectionTracker.LookupAndUpdate<ConnectionTracker::ConnectionDirectionInput>(packet,
                                                                                                       sessionSelector);
            if (!res) {
              BOOST_LOG_TRIVIAL(warning) << GetName()
                                         << ": LookupAndUpdate on input path failed: " << res.error().message();
            }
            return ErrorCode{};
          } else {
            return Error(AppErrorCategory::kEndOfStream);
          }
        }));
    if (queueFired.has_value()) {
      co_return queueFired.value();
    }
    if (cancelFired) {
      co_return Error(AppErrorCategory::kOperationAborted);
    }
    assert(false && "should not reach here");
    co_return ErrorCode{};
  }

  auto Write(Packet& packet, Cancel& cancel) -> Omni::Fiber::Coroutine<ErrorCode> override {
    if (cancel.IsTriggered()) {
      co_return Error(AppErrorCategory::kOperationAborted);
    }

    if (packet.HasMark()) {
      co_return co_await RouteByMark(packet, dynamic_cast<Mark&>(packet.GetMark()).GetValue());
    } else {
      auto route = _ConnectionTracker.LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(packet, _Selector);
      if (!route) {
        BOOST_LOG_TRIVIAL(debug) << GetName() << ": LookupAndUpdate failed: " << route.error().message()
                                 << ", dropping packet";
        co_return ErrorCode{};
      }
      co_return co_await RouteByMark(packet, std::dynamic_pointer_cast<Mark>(route.value())->GetValue());
    }
  }

protected:
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override { co_return ErrorCode{}; }
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override {
    _Pipe.GetConsumer().DiscardAndClose();
    co_return ErrorCode{};
  }

private:
  VpnClientMultiChannel& _Parent;
  ConnectionTracker& _ConnectionTracker;
  ConnectionTracker::Selector& _Selector;
  Omni::Fiber::Pipe<QueueEntry> _Pipe;

  auto RouteByMark(Packet& packet, const Mark::ValueType& value) const -> Omni::Fiber::Coroutine<ErrorCode> {
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
                   BOOST_LOG_TRIVIAL(debug) << GetName() << ": Packet marked Discard";
                   co_return ErrorCode{};
                 },
                 [&](const Mark::Deferred&) -> Omni::Fiber::Coroutine<ErrorCode> {
                   assert(false && "should not reach here");
                   std::unreachable();
                 },
                 [&](const Interface::VpnEndpoint& endpoint) -> Omni::Fiber::Coroutine<ErrorCode> {
                   if (auto session = endpoint.lock()) {
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
        value);
  }
};

auto VpnClientMultiChannel::ChannelSideEndpoint::Write(Packet& packet, Cancel& cancel)
    -> Omni::Fiber::Coroutine<ErrorCode> {
  if (cancel.IsTriggered()) {
    co_return Error(AppErrorCategory::kOperationAborted);
  }
  auto result = co_await _Parent._TunSide->PushIncoming(std::move(packet), _Session);
  if (!result.has_value()) {
    BOOST_LOG_TRIVIAL(info) << GetName() << " PushIncoming: TunSide is closed";
    co_return Error(AppErrorCategory::kOperationAborted);
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
    : _Executor(std::move(executor)), _Tun(std::move(tun)), _UdpDynMux(std::move(udpDynMux)),
      _ConnectionTracker(std::move(tracker)), _Filters(std::move(filters)), _StateListener(listener) {
  _TunSide = std::make_shared<TunSideEndpoint>(*this, *_ConnectionTracker, selector);
  _UdpDynMux->SetChannelNotification(*this);
}

VpnClientMultiChannel::~VpnClientMultiChannel() { assert(_Sessions.empty()); }

auto VpnClientMultiChannel::GetName() const -> std::string {
  return std::format("VpnClientMultiChannel:[{}]", _UdpDynMux->GetName());
}

auto VpnClientMultiChannel::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> {
  auto err = co_await _ConnectionTracker->Start();
  if (err) {
    BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to start ConnectionTracker: " << err.message();
    co_return err;
  }

  err = co_await _Tun->Start();
  if (err) {
    BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to start Tun: " << err.message();
    co_await _ConnectionTracker->Stop();
    co_return err;
  }

  err = co_await _UdpDynMux->Start();
  if (err) {
    BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to start UdpDynMux: " << err.message();
    co_await _Tun->Stop();
    co_await _ConnectionTracker->Stop();
    co_return err;
  }

  err = co_await _TunSide->Start();
  if (err) {
    BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to start TunSideEndpoint: " << err.message();
    co_await _UdpDynMux->Stop();
    co_await _Tun->Stop();
    co_await _ConnectionTracker->Stop();
    co_return err;
  }

  _TunPipeline = std::make_shared<Pipeline>(_Tun, std::vector<std::shared_ptr<Filter>>{}, _TunSide);
  err = co_await _TunPipeline->Start();
  if (err) {
    BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to start TUN pipeline: " << err.message();
    co_await _TunSide->Stop();
    co_await _UdpDynMux->Stop();
    co_await _Tun->Stop();
    co_await _ConnectionTracker->Stop();
    co_return err;
  }
  co_return ErrorCode{};
}

auto VpnClientMultiChannel::DoWork() -> Omni::Fiber::Coroutine<void> {
  BOOST_LOG_TRIVIAL(info) << GetName() << " run loop started";

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

  BOOST_LOG_TRIVIAL(info) << GetName() << " run loop finished";
}

auto VpnClientMultiChannel::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> {
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
  co_await _UdpDynMux->Stop();
  co_await _Tun->Stop();
  co_await _ConnectionTracker->Stop();
  _ChannelCall.DiscardAndClose();

  BOOST_LOG_TRIVIAL(info) << GetName() << " stopped";
  co_return ErrorCode{};
}

auto VpnClientMultiChannel::RegisterChannel(const UdpDynMux::PskType& psk, const std::string& address)
    -> Omni::Fiber::Coroutine<std::weak_ptr<VpnClientMultiChannelSession>> {
  auto [iterator, inserted] = _Sessions.emplace(psk, nullptr);
  if (inserted) {
    iterator->second = std::make_shared<VpnClientMultiChannelSession>();
  } else {
    co_return iterator->second;
  }
  _StateListener.get().OnSessionStarting(iterator->second);
  std::shared_ptr<ResolverEndpoint> resolver;
  if (!address.empty()) {
    resolver = FindResolverEndpoint(address, *_UdpDynMux);
  }
  iterator->second->Channel = co_await _UdpDynMux->CreateChannel(psk, resolver);
  if (!iterator->second->Channel) {
    _StateListener.get().OnSessionFailed(iterator->second, "Failed to create channel");
  }
  co_return iterator->second;
}

auto VpnClientMultiChannel::UnregisterChannel(std::weak_ptr<VpnClientMultiChannelSession> session)
    -> Omni::Fiber::Coroutine<void> {
  if (auto sharedSession = session.lock(); sharedSession) {
    _StateListener.get().OnSessionStopping(sharedSession);
    sharedSession->Running = false;
    if (sharedSession->Channel) {
      auto psk = sharedSession->Channel->GetPsk();
      co_await _UdpDynMux->RemoveChannel(psk);
      _StateListener.get().OnSessionStopped(sharedSession);
      _Sessions.erase(psk);
    } else {
      _StateListener.get().OnSessionStopped(sharedSession);
      std::erase_if(_Sessions, [&](const auto& pair) -> auto { return pair.second == sharedSession; });
    }
  }
}

auto VpnClientMultiChannel::OnChannelEstablished(std::shared_ptr<UdpDynMux::Channel> channel)
    -> Omni::Fiber::Coroutine<void> {
  BOOST_LOG_TRIVIAL(info) << GetName() << ": on channel established: " << channel->GetName();
  if (_State != State::kRunning) {
    co_return;
  }
  auto result =
      co_await _ChannelCall.Call([this, channel = std::move(channel)]() mutable -> Omni::Fiber::Coroutine<void> {
        if (_State != State::kRunning) {
          co_return;
        }
        auto iterator = _Sessions.find(channel->GetPsk());
        if (iterator != _Sessions.end()) {
          auto session = iterator->second;
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

auto VpnClientMultiChannel::OnChannelClosed(std::shared_ptr<UdpDynMux::Channel> channel)
    -> Omni::Fiber::Coroutine<void> {
  BOOST_LOG_TRIVIAL(info) << GetName() << ": on channel closed: " << channel->GetName();
  if (_State != State::kRunning) {
    co_return;
  }
  auto result =
      co_await _ChannelCall.Call([this, channel = std::move(channel)]() mutable -> Omni::Fiber::Coroutine<void> {
        if (_State != State::kRunning) {
          co_return;
        }
        auto iterator = _Sessions.find(channel->GetPsk());
        if (iterator != _Sessions.end()) {
          auto session = iterator->second;
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

auto VpnClientMultiChannel::MigrateTun(std::shared_ptr<Endpoint> newTun) -> Omni::Fiber::Coroutine<ErrorCode> {
  auto err = co_await _ChannelCall.Call([this, newTun]() -> Omni::Fiber::Coroutine<ErrorCode> {
    BOOST_LOG_TRIVIAL(info) << GetName() << ": migrating TUN endpoint";

    auto err = co_await newTun->Start();
    if (err) {
      BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to start new Tun during migration: " << err.message();
      co_return err;
    }

    if (_TunPipeline) {
      co_await _TunPipeline->Stop();
      _TunPipeline.reset();
    }

    if (_Tun) {
      co_await _Tun->Stop();
    }

    _Tun = newTun;
    _TunPipeline = std::make_shared<Pipeline>(_Tun, std::vector<std::shared_ptr<Filter>>{}, _TunSide);
    auto errPipeline = co_await _TunPipeline->Start();
    if (errPipeline) {
      BOOST_LOG_TRIVIAL(error) << GetName()
                               << ": Failed to start new TUN pipeline during migration: " << errPipeline.message();
      co_await _Tun->Stop();
      _Tun.reset();
      co_return errPipeline;
    }
    co_return ErrorCode{};
  });
  if (err.has_value()) {
    if (err.value()) {
      BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to migrate TUN endpoint: " << err.value().message();
    }
    co_return err.value();
  } else {
    BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to migrate TUN endpoint: _ChannelCall closed";
    co_return Error(AppErrorCategory::kOperationAborted);
  }
}

auto VpnClientMultiChannel::GetStats(const std::weak_ptr<VpnClientMultiChannelSession>& session)
    -> std::optional<VpnTrafficStats> {
  if (auto sharedSession = session.lock(); sharedSession) {
    if (sharedSession->SessionPipeline) {
      int64_t rttMs = -1;
      if (sharedSession->Channel) {
        rttMs = sharedSession->Channel->GetRoundTripTime().count();
      }
      return VpnTrafficStats{sharedSession->SessionPipeline->GetTrafficStats(), rttMs};
    }
  }
  return std::nullopt;
}

} // namespace gh
