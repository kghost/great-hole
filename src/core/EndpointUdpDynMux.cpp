#include "EndpointUdpDynMux.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <random>
#include <ranges>
#include <utility>

#include <boost/asio/buffer.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>

#include "Asio.hpp"
#include "BackoffTimer.hpp"
#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "GetCurrentFiber.hpp"
#include "Select.hpp"
#include "ServiceBase.hpp"

namespace gh {

// ==================== UdpDynMux::Channel ====================

UdpDynMux::Channel::Channel(UdpDynMux& parent, const UdpDynMux::PskType& psk, uint16_t rxId,
                            std::shared_ptr<ResolverEndpoint> resolver)
    : _Parent(parent), _Psk(psk), _LocalRxId(rxId), _PeerResolver(resolver) {}

UdpDynMux::Channel::~Channel() {}

std::string UdpDynMux::Channel::GetName() const {
  return std::format("UdpDynMuxChannel:{}:[psk_hash:{:02x}]", _Parent.GetName(), _Psk[0]);
}

Omni::Fiber::Coroutine<ErrorCode> UdpDynMux::Channel::DoStart() { co_return ErrorCode{}; }

Omni::Fiber::Coroutine<void> UdpDynMux::Channel::DoWork() {
  while (!_Service.value()._Stop.IsTriggered()) {
    switch (_State) {
    case State::kNegotiating: {
      _State = co_await DoWorkNegotiating();
      break;
    }
    case State::kRunning: {
      _State = co_await DoWorkRunning();
      break;
    }
    case State::kStopping: {
      co_return;
    }
    default: {
      assert(false && "should not reach here");
      co_return;
    }
    }
  }
  _State = State::kStopping;
}

Omni::Fiber::Coroutine<UdpDynMux::Channel::State> UdpDynMux::Channel::DoWorkNegotiating() {
  auto duration = BackoffTimerDuration(50, std::chrono::milliseconds(1000), std::chrono::milliseconds(2000),
                                       std::chrono::milliseconds(30000));
  while (!_Service.value()._Stop.IsTriggered()) {
    if (_Peer.has_value()) {
      BOOST_LOG_TRIVIAL(info) << GetName() << " negotiating sending initiate to " << *_Peer;
      co_await _Parent.SendControlInitiate(_Peer.value(), _Psk, _LocalRxId, _RemoteRxId);
      boost::asio::steady_timer timer(_Parent._Socket.get_executor());
      timer.expires_after(duration());
      auto [stopped, state, ec] = co_await Omni::Fiber::Select(
          Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(),
                                  [&]() { return ErrorCode{AppErrorCategory::kOperationAborted, kAppError}; }),
          Omni::Fiber::SelectPair(_ControlPacket.GetConsumer(),
                                  [&](auto info) -> Omni::Fiber::Coroutine<UdpDynMux::Channel::State> {
                                    assert(info.has_value() && "Pipe should never ends");
                                    auto [peer, packet] = std::move(info.value());
                                    // FIXME: there are gcc bugs which causes packet object leaks
                                    co_return co_await HandleControlPacket(peer, packet);
                                  }),
          Omni::Fiber::SelectPair(timer.async_wait(boost::asio::bind_cancellation_slot(
                                      _Service.value()._Stop.AsioSlot().Slot(), Omni::Fiber::AsioUseFiber)),
                                  Omni::Fiber::AsioApply([](auto ec) { return ec; })));
      if (state.has_value()) {
        if (state.value() != State::kNegotiating) {
          co_return state.value();
        }
      }
    } else if (_PeerResolver) {
      // TODO: resolver need to be cancellable
      BOOST_LOG_TRIVIAL(info) << GetName() << " resolving peer";
      auto res = co_await _PeerResolver->Resolve();
      if (res.has_value()) {
        BOOST_LOG_TRIVIAL(info) << GetName() << " resolved peer endpoint: " << res.value();
        _Peer = res.value();
      } else {
        BOOST_LOG_TRIVIAL(warning) << GetName() << " peer resolution failed: " << res.error().message();
      }
    } else {
      BOOST_LOG_TRIVIAL(info) << GetName() << " negotiating waiting for peer endpoint initiate";
      auto [stopped, state] = co_await Omni::Fiber::Select(
          Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(),
                                  [&]() { return ErrorCode{AppErrorCategory::kOperationAborted, kAppError}; }),
          Omni::Fiber::SelectPair(_ControlPacket.GetConsumer(),
                                  [&](auto info) -> Omni::Fiber::Coroutine<UdpDynMux::Channel::State> {
                                    assert(info.has_value() && "Pipe should never ends");
                                    auto [peer, packet] = std::move(info.value());
                                    // FIXME: there are gcc bugs which causes packet object leaks
                                    co_return co_await HandleControlPacket(peer, packet);
                                  }));
      if (state.has_value()) {
        if (state.value() != State::kNegotiating) {
          co_return state.value();
        }
      }
    }
  }
  co_return State::kStopping;
}

Omni::Fiber::Coroutine<UdpDynMux::Channel::State> UdpDynMux::Channel::DoWorkRunning() {
  while (!_Service.value()._Stop.IsTriggered()) {
    auto now = std::chrono::steady_clock::now();
    if (now - _LastSeen > std::chrono::seconds(180)) {
      BOOST_LOG_TRIVIAL(warning) << GetName() << " session timeout, resetting to negotiating";
      _Peer = std::nullopt;
      _RemoteRxId = 0;
      co_return State::kNegotiating;
    }

    if (now >= _NextKeepaliveTime) {
      co_await _Parent.SendControlKeepalive(_Peer.value(), _Psk);
      std::uniform_int_distribution<int> dist(30, 60);
      _NextKeepaliveTime = now + std::chrono::seconds(dist(_Parent._Prng));
    }

    boost::asio::steady_timer timer(_Parent._Socket.get_executor());
    timer.expires_at(_NextKeepaliveTime);

    auto [stopped, state, ec] = co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(),
                                [&]() { return ErrorCode{AppErrorCategory::kOperationAborted, kAppError}; }),
        Omni::Fiber::SelectPair(_ControlPacket.GetConsumer(),
                                [&](auto info) -> Omni::Fiber::Coroutine<UdpDynMux::Channel::State> {
                                  assert(info.has_value() && "Pipe should never ends");
                                  auto [peer, packet] = std::move(info.value());
                                  // FIXME: there are gcc bugs which causes packet object leaks
                                  co_return co_await HandleControlPacket(peer, packet);
                                }),
        Omni::Fiber::SelectPair(timer.async_wait(boost::asio::bind_cancellation_slot(
                                    _Service.value()._Stop.AsioSlot().Slot(), Omni::Fiber::AsioUseFiber)),
                                Omni::Fiber::AsioApply([](auto ec) { return ec; })));
    if (state.has_value() && state.value() != State::kRunning) {
      co_return state.value();
    }
  }

  co_return State::kStopping;
}

Omni::Fiber::Coroutine<ErrorCode> UdpDynMux::Channel::DoGracefulStop() {
  co_await _PipielineUsageCounter.WaitAll();
  co_await _DataPacket.GetProducer().Close();
  co_await _ControlPacket.GetProducer().Close();
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> UdpDynMux::Channel::Read(Packet& p, Cancel& c) {
  auto [stopped, err] =
      co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(c.GetFiberCancelEvent(), [] {}),
                                   Omni::Fiber::SelectPair(_DataPacket.GetConsumer(), [&](auto data) -> ErrorCode {
                                     if (data.has_value()) {
                                       _LastSeen = std::chrono::steady_clock::now();
                                       p = std::move(data.value());
                                       p.PopFront(2);
                                       return ErrorCode{};
                                     } else {
                                       p._Length = 0;
                                       return ErrorCode{AppErrorCategory::kEndOfStream, kAppError};
                                     }
                                   }));
  if (err.has_value()) {
    co_return err.value();
  }
  if (stopped) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }
  assert(false && "should not reach here");
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> UdpDynMux::Channel::Write(Packet& p, Cancel& c) {
  if (c.IsTriggered() || ServiceBase::_State != ServiceBase::State::kRunning) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }

  if (_State != State::kRunning) {
    co_return ErrorCode{AppMinorErrorCategory::kInvalidPacketSession, kAppMinorError};
  }

  if (!_Peer.has_value() || _RemoteRxId == 0) {
    co_return ErrorCode{AppMinorErrorCategory::kInvalidPacketSession, kAppMinorError};
  }

  if (p._Offset < 2) {
    co_return ErrorCode{AppErrorCategory::kInvalidPacketReserved, kAppError};
  }

  p.PushFront(_RemoteRxId);

  co_return co_await _Parent.WriteTo(_Peer.value(), p, c);
}

Omni::Fiber::Coroutine<UdpDynMux::Channel::State>
UdpDynMux::Channel::HandleControlPacket(boost::asio::ip::udp::endpoint peer, Packet& packet) {
  uint16_t channelId = UdpDynMuxProto::ReadUint16Be(packet._Data.data() + packet._Offset);
  UdpDynMuxProto::MsgType msgType(UdpDynMuxProto::MsgType(packet._Data[packet._Offset + 2]));
  auto now = std::chrono::steady_clock::now();

  if (msgType == static_cast<uint8_t>(UdpDynMuxProto::MsgType::kInitiate)) {
    if (auto init = UdpDynMuxProto::Initiate::Deserialize(packet.Data())) {
      bool myRxMatches = (init->PeerRxId == _LocalRxId);
      bool peerRxMatches = (init->RxId == _RemoteRxId && _Peer == peer);
      _LastSeen = now;
      if (!peerRxMatches) {
        BOOST_LOG_TRIVIAL(info) << GetName() << " received initiate (tx mismatch) from " << peer;
        _RemoteRxId = init->RxId;
        _Peer = peer; // peer address is strictly updated only on receiving INITIATE
      } else {
        BOOST_LOG_TRIVIAL(info) << GetName() << " received initiate (tx matched) from " << peer;
      }

      if (!myRxMatches) {
        BOOST_LOG_TRIVIAL(info) << GetName() << " received initiate (my rx mismatch) sending initiate to " << peer;
        co_await _Parent.SendControlInitiate(peer, init->Psk, _LocalRxId, _RemoteRxId);
      }

      co_return State::kRunning;
    }
  } else if (msgType == static_cast<uint8_t>(UdpDynMuxProto::MsgType::kKeepalive)) {
    if (auto ping = UdpDynMuxProto::Keepalive::Deserialize(packet.Data())) {
      if (_Peer == peer) {
        _LastSeen = now;
        co_await _Parent.SendControlKeepaliveAck(peer, _Psk);
      } else {
        co_await _Parent.SendControlInvalidChannel(peer, _LocalRxId);
      }
    }
  } else if (msgType == static_cast<uint8_t>(UdpDynMuxProto::MsgType::kKeepaliveAck)) {
    if (auto ack = UdpDynMuxProto::KeepaliveAck::Deserialize(packet.Data())) {
      if (_Peer == peer) {
        _LastSeen = now;
      } else {
        co_await _Parent.SendControlInvalidChannel(peer, _LocalRxId);
      }
    }
  } else if (msgType == static_cast<uint8_t>(UdpDynMuxProto::MsgType::kInvalidChannel)) {
    if (auto err = UdpDynMuxProto::InvalidChannel::Deserialize(packet.Data())) {
      if (_Peer == peer) {
        // upon receiving INVALID_CHANNEL, if peer id and address matches, re kResolving, if not match silent drop
        BOOST_LOG_TRIVIAL(warning) << GetName() << " received INVALID_CHANNEL, resetting state to negotiating";
        _Peer = std::nullopt;
        _RemoteRxId = 0;
        co_return State::kNegotiating;
      }
    }
  }
  co_return _State;
}

// ==================== UdpDynMux ====================

UdpDynMux::UdpDynMux(boost::asio::io_context& ioContext, ChannelNotification& notification)
    : UdpDynMux(ioContext, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v6(), 0), notification) {}

UdpDynMux::UdpDynMux(boost::asio::io_context& ioContext, boost::asio::ip::udp::endpoint bind,
                     ChannelNotification& notification)
    : _Notification(notification), _Socket(ioContext), _Local(bind) {
  std::random_device rd;
  _Prng.seed(rd());
}

UdpDynMux::~UdpDynMux() { assert(_Channels.empty()); }

std::string UdpDynMux::GetName() const { return "UdpDynMux:" + boost::lexical_cast<std::string>(_Local); }

Omni::Fiber::Coroutine<ErrorCode> UdpDynMux::DoStart() {
  ErrorCode ec;
  try {
    _Socket.open(boost::asio::ip::udp::v6());
    _Socket.set_option(boost::asio::ip::v6_only(false));
    _Socket.bind(_Local);
    _Local = _Socket.local_endpoint();
    BOOST_LOG_TRIVIAL(info) << GetName() << " bound at " << _Local;
  } catch (const SystemError& e) {
    BOOST_LOG_TRIVIAL(info) << GetName() << " start failed: " << e.what();
    ec = e.code();
  }

  if (ec) {
    co_await _ChannelRpc.Close();
    co_return ec;
  }
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<void> UdpDynMux::DoWork() {
  auto& currentFiber = co_await Omni::Fiber::GetCurrentFiber();
  currentFiber.Spawn(GetName() + " ReadLoop", [this]() -> Omni::Fiber::Coroutine<void> {
    co_await ReadLoop();
    co_return;
  });

  bool stopped = false;
  while (!stopped) {
    co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(), [&]() { stopped = true; }),
        Omni::Fiber::SelectPair(_ChannelRpc.GetServiceAwaitor(), [&](auto req) -> Omni::Fiber::Coroutine<void> {
          if (!co_await Omni::Fiber::RemoteCall::HandleRequest(std::move(req))) {
            stopped = true;
          }
          co_return;
        }));
  }
}

Omni::Fiber::Coroutine<ErrorCode> UdpDynMux::DoGracefulStop() {
  co_await _ChannelRpc.Close();
  for (auto& [psk, channel] : std::exchange(_Channels, {})) {
    co_await channel->Stop();
    co_await channel->WaitService();
  }
  _RxIdToChannel.clear();
  co_await (co_await Omni::Fiber::GetCurrentFiber()).WaitAll();
  _Socket.close();
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<std::shared_ptr<UdpDynMux::Channel>> UdpDynMux::CreateChannel(const UdpDynMux::PskType& psk) {
  co_return co_await CreateChannel(psk, nullptr);
}

Omni::Fiber::Coroutine<std::shared_ptr<UdpDynMux::Channel>>
UdpDynMux::CreateChannel(const UdpDynMux::PskType& psk, std::shared_ptr<ResolverEndpoint> resolver) {
  auto reply = co_await _ChannelRpc.Call(
      [&udp = *this, psk, resolver](this auto self) -> Omni::Fiber::Coroutine<std::shared_ptr<Channel>> {
        auto channel = std::make_shared<Channel>(udp, psk, udp.AllocateUniqueRxId(), resolver);
        channel->_LastSeen = std::chrono::steady_clock::now();
        channel->_NextKeepaliveTime = std::chrono::steady_clock::now() + std::chrono::seconds(5);

        auto [it, inserted] = udp._Channels.try_emplace(psk, channel);
        assert(inserted);
        auto [it2, inserted2] = udp._RxIdToChannel.try_emplace(channel->_LocalRxId, channel);
        assert(inserted2);

        auto err = co_await channel->Start();
        if (err) {
          udp._RxIdToChannel.erase(channel->_LocalRxId);
          udp._Channels.erase(psk);
          co_await channel->WaitService();
          co_return nullptr;
        }

        co_return channel;
      });
  assert(reply.has_value());
  co_return reply.value();
}

Omni::Fiber::Coroutine<void> UdpDynMux::RemoveChannel(const UdpDynMux::PskType& psk) {
  co_await _ChannelRpc.Call([this, psk]() -> Omni::Fiber::Coroutine<void> {
    auto it = _Channels.find(psk);
    if (it != _Channels.end()) {
      auto channel = std::move(it->second);
      _RxIdToChannel.erase(channel->_LocalRxId);
      _Channels.erase(it);
      co_await channel->Stop();
      co_await channel->WaitService();
    }
  });
}

Omni::Fiber::Coroutine<void> UdpDynMux::ReadLoop() {
  auto slotTracker = _Service.value()._Stop.AsioSlot();
  while (!_Service.value()._Stop.IsTriggered()) {
    Packet packet;
    boost::asio::ip::udp::endpoint peer;

    auto [err, bytes_transferred] = co_await _Socket.async_receive_from(
        boost::asio::mutable_buffer(packet), peer,
        boost::asio::bind_cancellation_slot(slotTracker.Slot(), Omni::Fiber::AsioUseFiber));
    if (err) {
      if (err == boost::asio::error::operation_aborted) {
        BOOST_LOG_TRIVIAL(info) << GetName() << ": read loop cancelled";
      } else {
        BOOST_LOG_TRIVIAL(error) << GetName() << ": read error: " << err.message();
        for (auto& [psk, channel] : _Channels) {
          if (channel->_State == Channel::State::kRunning) {
            // TODO: UDP read error, should we notify channels? currently just stop all channels, which will cause all
            // pending operations to fail with operation_aborted, and upper layer can decide how to handle it (e.g. try
            // to re-establish channel, or just give up)
          }
        }
      }
      break;
    }

    packet._Length = bytes_transferred;

    if (bytes_transferred < 2) {
      BOOST_LOG_TRIVIAL(info) << GetName() << ": ignored empty/short packet";
      continue;
    }

    uint16_t channelId = UdpDynMuxProto::ReadUint16Be(packet._Data.data() + packet._Offset);
    if (channelId == 0) {
      // Control packet
      if (bytes_transferred >= 3) {
        UdpDynMuxProto::MsgType msgType(UdpDynMuxProto::MsgType(packet._Data[packet._Offset + 2]));
        switch (msgType) {
        case UdpDynMuxProto::MsgType::kInitiate:
        case UdpDynMuxProto::MsgType::kKeepalive:
        case UdpDynMuxProto::MsgType::kKeepaliveAck:
        case UdpDynMuxProto::MsgType::kInvalidPsk: {
          if (bytes_transferred < 19) {
            break;
          }
          auto psk = UdpDynMuxProto::ReadPsk(packet._Data.data() + packet._Offset + 3);
          auto it = _Channels.find(psk);
          if (it == _Channels.end()) {
            break;
          }
          auto channel = it->second;
          co_await channel->_ControlPacket.GetProducer().Put(std::make_tuple(peer, std::move(packet)));
          break;
        }
        case UdpDynMuxProto::MsgType::kInvalidChannel: {
          if (auto err = UdpDynMuxProto::InvalidChannel::Deserialize(packet.Data())) {
            // TODO: find a way to look by id or peer, currently just broadcast to all channels, and let channel decide
            // whether it's for them or not, which is not efficient but should be fine since this should be a rare event
            for (auto& [psk, channel] : _Channels) {
              if (channel->_RemoteRxId == err->ChannelId && channel->_Peer == peer) {
                co_await channel->_ControlPacket.GetProducer().Put(std::make_tuple(peer, std::move(packet)));
              }
            }
          }
          break;
        }
        default:
          assert(false);
        }
      }
    } else {
      if (auto it = _RxIdToChannel.find(channelId); it != _RxIdToChannel.end()) {
        auto channel = it->second;
        if (channel->_State == Channel::State::kRunning && channel->_Peer == peer) {
          co_await channel->_DataPacket.GetProducer().Put(std::move(packet));
        } else {
          co_await SendControlInvalidChannel(peer, channelId);
        }
      } else {
        co_await SendControlInvalidChannel(peer, channelId);
      }
    }
  }
}

Omni::Fiber::Coroutine<ErrorCode> UdpDynMux::WriteTo(boost::asio::ip::udp::endpoint peer, Packet& p, Cancel& c) {
  auto [err, bytes_transferred] = co_await _Socket.async_send_to(
      boost::asio::const_buffer(p), peer,
      boost::asio::bind_cancellation_slot(c.AsioSlot().Slot(), Omni::Fiber::AsioUseFiber));
  assert(err || bytes_transferred == p._Length);
  co_return err;
}

bool UdpDynMux::CheckRateLimit(const boost::asio::ip::udp::endpoint& peer) {
  auto now = std::chrono::steady_clock::now();
  std::erase_if(_LastErrorSent, [&](const auto& entry) { return now - entry.second > std::chrono::seconds(10); });
  if (auto it = _LastErrorSent.find(peer); it != _LastErrorSent.end() && now - it->second < std::chrono::seconds(1)) {
    return false;
  }
  _LastErrorSent[peer] = now;
  return true;
}

template <typename T>
static Omni::Fiber::Coroutine<void> SendControlPacket(boost::asio::ip::udp::socket& socket,
                                                      const boost::asio::ip::udp::endpoint& peer, const T& msg) {
  auto buf = std::make_shared<std::array<uint8_t, T::kSize>>();
  msg.Serialize(*buf);
  co_await socket.async_send_to(boost::asio::buffer(*buf), peer, Omni::Fiber::AsioUseFiber);
}

Omni::Fiber::Coroutine<void> UdpDynMux::SendControlInitiate(const boost::asio::ip::udp::endpoint& peer,
                                                            const UdpDynMux::PskType& psk, uint16_t rxId,
                                                            uint16_t peerRxId) {
  co_await SendControlPacket(_Socket, peer, UdpDynMuxProto::Initiate{psk, rxId, peerRxId});
}

Omni::Fiber::Coroutine<void> UdpDynMux::SendControlKeepalive(const boost::asio::ip::udp::endpoint& peer,
                                                             const UdpDynMux::PskType& psk) {
  co_await SendControlPacket(_Socket, peer, UdpDynMuxProto::Keepalive{psk});
}

Omni::Fiber::Coroutine<void> UdpDynMux::SendControlKeepaliveAck(const boost::asio::ip::udp::endpoint& peer,
                                                                const UdpDynMux::PskType& psk) {
  co_await SendControlPacket(_Socket, peer, UdpDynMuxProto::KeepaliveAck{psk});
}

Omni::Fiber::Coroutine<void> UdpDynMux::SendControlInvalidPsk(const boost::asio::ip::udp::endpoint& peer,
                                                              const UdpDynMux::PskType& psk) {
  if (!CheckRateLimit(peer)) {
    co_return;
  }
  co_await SendControlPacket(_Socket, peer, UdpDynMuxProto::InvalidPsk{psk});
}

Omni::Fiber::Coroutine<void> UdpDynMux::SendControlInvalidChannel(const boost::asio::ip::udp::endpoint& peer,
                                                                  uint16_t channelId) {
  if (!CheckRateLimit(peer)) {
    co_return;
  }
  co_await SendControlPacket(_Socket, peer, UdpDynMuxProto::InvalidChannel{channelId});
}

uint16_t UdpDynMux::AllocateUniqueRxId() {
  std::uniform_int_distribution<uint16_t> dist(1, 65535);
  while (true) {
    uint16_t candidate = dist(_Prng);
    if (candidate != 0 && _RxIdToChannel.find(candidate) == _RxIdToChannel.end()) {
      return candidate;
    }
  }
}

UdpDynMux::NoopChannelNotification UdpDynMux::_NoopChannelNotification;

} // namespace gh
