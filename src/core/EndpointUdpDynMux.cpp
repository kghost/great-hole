#include "EndpointUdpDynMux.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
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
#include "Cancel.hpp"
#include "ErrorCode.hpp"
#include "GetCurrentFiber.hpp"
#include "Select.hpp"

namespace gh {

// ==================== UdpDynMux::Channel ====================

UdpDynMux::Channel::Channel(UdpDynMux& parent, const UdpDynMux::PskType& psk, uint16_t rxId,
                            std::shared_ptr<ResolverEndpoint> resolver)
    : _Parent(parent), _Psk(psk), _LocalRxId(rxId), _PeerResolver(resolver) {}

UdpDynMux::Channel::~Channel() {}

std::string UdpDynMux::Channel::GetName() const { return std::format("UdpDynMuxChannel:[psk_hash:{:02x}]", _Psk[0]); }

Omni::Fiber::Coroutine<ErrorCode> UdpDynMux::Channel::DoStart() {
  if (_PeerResolver) {
    auto res = co_await _PeerResolver->Resolve();
    if (res.has_value()) {
      BOOST_LOG_TRIVIAL(info) << "(" << _Parent.GetName() << "):(" << GetName()
                              << ") resolved peer endpoint immediately: " << res.value();
      if (!_Peer.has_value()) {
        _Peer = res.value();
        co_await _Parent.SendControlInitiate(*_Peer, _Psk, _LocalRxId, _RemoteRxId);
      }
    } else {
      BOOST_LOG_TRIVIAL(warning) << "(" << _Parent.GetName() << "):(" << GetName()
                                 << ") initial peer resolution failed: " << res.error().message();
    }
  }
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> UdpDynMux::Channel::DoGracefulStop() {
  co_await _PipielineUsageCounter.WaitAll();
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> UdpDynMux::Channel::Read(Packet& p, Cancel& c) {
  bool stopped = false;
  std::optional<ErrorCode> err;
  co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(c.GetFiberCancelEvent(), [&]() { stopped = true; }),
                               Omni::Fiber::SelectPair(_Pipe.GetConsumer(), [&](auto data) {
                                 if (data.has_value()) {
                                   auto& inner = data.value();
                                   if (inner.has_value()) {
                                     p = std::move(inner.value());
                                     err = ErrorCode{};
                                   } else {
                                     err = inner.error();
                                   }
                                 } else {
                                   p._Length = 0;
                                   err = ErrorCode{AppErrorCategory::kEndOfStream, kAppError};
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
  if (c.IsTriggered() || IsStopped()) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }

  if (_State != kRunning) {
    co_return ErrorCode{AppErrorCategory::kInvalidPacketSession, kAppError};
  }

  if (!_Peer.has_value() || _RemoteRxId == 0) {
    co_return ErrorCode{AppErrorCategory::kInvalidPacketSession, kAppError};
  }

  co_return co_await _Parent.WriteTo(*this, p, c);
}

// ==================== UdpDynMux ====================

UdpDynMux::UdpDynMux(boost::asio::io_context& ioContext) : _Socket(ioContext), _Local(boost::asio::ip::udp::v6(), 0) {
  std::random_device rd;
  _Prng.seed(rd());
}

UdpDynMux::UdpDynMux(boost::asio::io_context& ioContext, boost::asio::ip::udp::endpoint bind)
    : _Socket(ioContext), _Local(bind) {
  std::random_device rd;
  _Prng.seed(rd());
}

UdpDynMux::~UdpDynMux() { assert(_Channels.empty()); }

std::string UdpDynMux::GetName() const { return "UdpDynMux:" + boost::lexical_cast<std::string>(_Local); }

Omni::Fiber::Coroutine<ErrorCode> UdpDynMux::DoStart() {
  try {
    _Socket.open(boost::asio::ip::udp::v6());
    _Socket.set_option(boost::asio::ip::v6_only(false));
    _Socket.bind(_Local);
    BOOST_LOG_TRIVIAL(info) << "(" << GetName() << ") bound at " << _Socket.local_endpoint();
  } catch (const SystemError& e) {
    BOOST_LOG_TRIVIAL(info) << "(" << GetName() << ") start failed: " << e.what();
    co_return e.code();
  }
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<void> UdpDynMux::DoWork() {
  auto& currentFiber = co_await Omni::Fiber::GetCurrentFiber();
  currentFiber.Spawn(GetName() + " ReadLoop", [this]() -> Omni::Fiber::Coroutine<void> {
    co_await ReadLoop();
    co_return;
  });

  currentFiber.Spawn(GetName() + " ControlLoop", [this]() -> Omni::Fiber::Coroutine<void> {
    co_await ControlLoop();
    co_return;
  });

  bool stopped = false;
  while (!stopped) {
    co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(_Stop.GetFiberCancelEvent(), [&]() { stopped = true; }),
        Omni::Fiber::SelectPair(_ChannelRpc.GetServiceAwaitor(), [](auto req) -> Omni::Fiber::Coroutine<void> {
          bool ok = co_await Omni::Fiber::RemoteCall::HandleRequest(std::move(req));
          assert(ok);
          co_return;
        }));
  }
}

Omni::Fiber::Coroutine<ErrorCode> UdpDynMux::DoGracefulStop() {
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
  co_return co_await _ChannelRpc.Call(
      [this, psk, resolver](this auto self) -> Omni::Fiber::Coroutine<std::shared_ptr<Channel>> {
        auto channel = std::make_shared<Channel>(*this, psk, AllocateUniqueRxId(), resolver);
        channel->_LastSeen = std::chrono::steady_clock::now();
        channel->_NextKeepaliveTime = std::chrono::steady_clock::now() + std::chrono::seconds(5);

        auto [it, inserted] = _Channels.try_emplace(psk, channel);
        assert(inserted);
        auto [it2, inserted2] = _RxIdToChannel.try_emplace(channel->_LocalRxId, channel);
        assert(inserted2);

        auto err = co_await channel->Start();
        if (err) {
          _RxIdToChannel.erase(channel->_LocalRxId);
          _Channels.erase(psk);
          co_await channel->WaitService();
          co_return nullptr;
        }

        co_return channel;
      });
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
  auto slotTracker = _Stop.AsioSlot();
  while (!_Stop.IsTriggered()) {
    Packet p;
    boost::asio::ip::udp::endpoint peer;

    auto [err, bytes_transferred] = co_await _Socket.async_receive_from(
        boost::asio::mutable_buffer(p), peer,
        boost::asio::bind_cancellation_slot(slotTracker.Slot(), Omni::Fiber::AsioUseFiber));
    if (err) {
      if (err == boost::asio::error::operation_aborted) {
        BOOST_LOG_TRIVIAL(info) << GetName() << ": read loop cancelled";
      } else {
        BOOST_LOG_TRIVIAL(error) << GetName() << ": read error: " << err.message();
        for (auto& [psk, channel] : _Channels) {
          if (!channel->IsStopped()) {
            co_await channel->Send(std::unexpected(err));
          }
        }
      }
      break;
    }

    if (bytes_transferred < 2) {
      BOOST_LOG_TRIVIAL(info) << GetName() << ": ignored empty/short packet";
      continue;
    }

    uint16_t channelId = UdpDynMuxProto::ReadUint16Be(p._Data.data() + p._Offset);
    auto now = std::chrono::steady_clock::now();

    if (channelId == 0) {
      // Control packet
      if (bytes_transferred >= 3) {
        uint8_t msgType = p._Data[p._Offset + 2];
        if (msgType == static_cast<uint8_t>(UdpDynMuxProto::MsgType::kInitiate)) {
          if (auto req = UdpDynMuxProto::Initiate::Deserialize(p._Data.data() + p._Offset, bytes_transferred)) {
            auto it = _Channels.find(req->Psk);
            if (it != _Channels.end()) {
              auto ch = it->second;
              bool myRxMatches = (req->PeerRxId == ch->_LocalRxId);
              bool peerRxMatches = (req->RxId == ch->_RemoteRxId && ch->_Peer == peer);

              if (ch->_State != Channel::kRunning) {
                ch->_State = Channel::kRunning;
              }

              ch->_LastSeen = now;
              ch->_MissingAcks = 0;
              std::uniform_int_distribution<int> dist(5000, 10000);
              ch->_NextKeepaliveTime = now + std::chrono::milliseconds(dist(_Prng));

              if (!peerRxMatches) {
                BOOST_LOG_TRIVIAL(info) << GetName() << ":(" << ch->GetName() << ") channel updated (rx mismatch) to "
                                        << peer;
                ch->_RemoteRxId = req->RxId;
                ch->_Peer = peer; // peer address is strictly updated only on receiving INITIATE
              } else {
                BOOST_LOG_TRIVIAL(info) << GetName() << ":(" << ch->GetName() << ") channel running (rx matched) to "
                                        << peer;
              }

              if (!myRxMatches) {
                BOOST_LOG_TRIVIAL(info) << GetName() << ":(" << ch->GetName()
                                        << ") channel sending initiate (rx mismatch) to " << peer;
                co_await SendControlInitiate(peer, req->Psk, ch->_LocalRxId, ch->_RemoteRxId);
              }
            } else {
              BOOST_LOG_TRIVIAL(info) << GetName() << " channel sending invalid psk to " << peer;
              co_await SendControlInvalidPsk(peer, req->Psk);
            }
          }
        } else if (msgType == static_cast<uint8_t>(UdpDynMuxProto::MsgType::kKeepalive)) {
          if (auto ping = UdpDynMuxProto::Keepalive::Deserialize(p._Data.data() + p._Offset, bytes_transferred)) {
            auto it = _Channels.find(ping->Psk);
            if (it != _Channels.end()) {
              auto ch = it->second;
              if (ch->_Peer == peer) {
                ch->_LastSeen = now;
                co_await SendControlKeepaliveAck(peer, ch->_Psk);

                // Restart our own keepalive timer
                std::uniform_int_distribution<int> dist(5000, 10000);
                ch->_NextKeepaliveTime = now + std::chrono::milliseconds(dist(_Prng));
              } else {
                co_await SendControlInvalidChannel(peer, ch->_LocalRxId);
              }
            } else {
              co_await SendControlInvalidPsk(peer, ping->Psk);
            }
          }
        } else if (msgType == static_cast<uint8_t>(UdpDynMuxProto::MsgType::kKeepaliveAck)) {
          if (auto ack = UdpDynMuxProto::KeepaliveAck::Deserialize(p._Data.data() + p._Offset, bytes_transferred)) {
            auto it = _Channels.find(ack->Psk);
            if (it != _Channels.end()) {
              auto ch = it->second;
              if (ch->_Peer == peer) {
                ch->_LastSeen = now;
                ch->_MissingAcks = 0;
              } else {
                co_await SendControlInvalidChannel(peer, ch->_LocalRxId);
              }
            } else {
              co_await SendControlInvalidPsk(peer, ack->Psk);
            }
          }
        } else if (msgType == static_cast<uint8_t>(UdpDynMuxProto::MsgType::kInvalidChannel)) {
          if (auto err = UdpDynMuxProto::InvalidChannel::Deserialize(p._Data.data() + p._Offset, bytes_transferred)) {
            // upon receiving INVALID_CHANNEL, look up peer id peer address, if matches, re INITIATE, if does not match,
            // silent drop
            for (auto& [psk, ch] : _Channels) {
              if (ch->_RemoteRxId == err->ChannelId && ch->_Peer == peer) {
                BOOST_LOG_TRIVIAL(warning) << GetName() << ":(" << ch->GetName()
                                           << ") received INVALID_CHANNEL, resetting state to negotiating";
                ch->_State = Channel::kNegotiating;
                co_await SendControlInitiate(peer, psk, ch->_LocalRxId, ch->_RemoteRxId);
              }
            }
          }
        }
      }
    } else {
      // Data packet
      auto it = _RxIdToChannel.find(channelId);
      if (it != _RxIdToChannel.end()) {
        auto ch = it->second;
        if (ch->_State == Channel::kRunning && ch->_Peer == peer) {
          ch->_LastSeen = now;
          ch->_MissingAcks = 0;
          p._Offset += 2;
          p._Length = bytes_transferred - 2;
          co_await ch->Send(std::move(p));
        } else {
          co_await SendControlInvalidChannel(peer, channelId);
        }
      } else {
        co_await SendControlInvalidChannel(peer, channelId);
      }
    }
  }
}

Omni::Fiber::Coroutine<void> UdpDynMux::ControlLoop() {
  // TODO: move this into channel fiber
  auto slotTracker = _Stop.AsioSlot();
  boost::asio::steady_timer timer(_Socket.get_executor());

  while (!_Stop.IsTriggered()) {
    auto now = std::chrono::steady_clock::now();
    for (auto& [psk, ch] : _Channels) {
      if (ch->_PeerResolver && !ch->_Peer.has_value()) {
        auto res = co_await ch->_PeerResolver->Resolve();
        if (res.has_value()) {
          ch->_Peer = res.value();
          BOOST_LOG_TRIVIAL(info) << GetName() << ": resolved peer endpoint: " << *ch->_Peer;
        } else {
          BOOST_LOG_TRIVIAL(warning) << GetName() << ": peer resolution failed: " << res.error().message();
          continue;
        }
      }

      if (ch->_Peer.has_value()) {
        auto peer = *ch->_Peer;
        if (ch->_State == Channel::kNegotiating) {
          co_await SendControlInitiate(peer, psk, ch->_LocalRxId, ch->_RemoteRxId);
        } else if (ch->_State == Channel::kRunning) {
          if (now - ch->_LastSeen > std::chrono::seconds(15)) {
            BOOST_LOG_TRIVIAL(warning) << "UdpDynMux session timeout, resetting to negotiating";
            ch->_State = Channel::kNegotiating;
            ch->_MissingAcks = 0;
            if (!ch->_PeerResolver) {
              ch->_Peer = std::nullopt;
            }
            ch->_RemoteRxId = 0;
          } else {
            if (now >= ch->_NextKeepaliveTime) {
              co_await SendControlKeepalive(peer, psk);
              ch->_MissingAcks++;

              std::uniform_int_distribution<int> dist(5000, 10000);
              ch->_NextKeepaliveTime = now + std::chrono::milliseconds(dist(_Prng));
            }
          }
        }
      }
    }

    timer.expires_after(std::chrono::seconds(1));
    auto [err] =
        co_await timer.async_wait(boost::asio::bind_cancellation_slot(slotTracker.Slot(), Omni::Fiber::AsioUseFiber));
    if (err) {
      break;
    }
  }
}

Omni::Fiber::Coroutine<ErrorCode> UdpDynMux::WriteTo(Channel& ch, Packet& p, Cancel& c) {
  if (p._Offset < 2) {
    co_return ErrorCode{AppErrorCategory::kInvalidPacketReserved, kAppError};
  }

  p._Offset -= 2;
  p._Length += 2;
  UdpDynMuxProto::WriteUint16Be(p._Data.data() + p._Offset, ch._RemoteRxId);

  auto [err, bytes_transferred] = co_await _Socket.async_send_to(
      boost::asio::const_buffer(p), *ch._Peer,
      boost::asio::bind_cancellation_slot(c.AsioSlot().Slot(), Omni::Fiber::AsioUseFiber));
  assert(err || bytes_transferred == p._Length);
  co_return err;
}

bool UdpDynMux::CheckRateLimit(const boost::asio::ip::udp::endpoint& peer) {
  auto now = std::chrono::steady_clock::now();
  auto it = _LastErrorSent.find(peer);
  if (it != _LastErrorSent.end() && now - it->second < std::chrono::seconds(1)) {
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

} // namespace gh
