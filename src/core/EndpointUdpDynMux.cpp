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
#include "EndpointUdpDynMuxProtocol.hpp"
#include "ErrorCode.hpp"
#include "GetCurrentFiber.hpp"
#include "PacketBuilder.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"
#include "ServiceBase.hpp"
#include "Utils/Overload.hpp"

namespace gh {

using namespace gh::UdpDynMuxProto;

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
  auto lastState = _State;
  while (!_Service.value()._Stop.IsTriggered()) {
    switch (_State) {
    case State::kNegotiating: {
      _State = co_await DoWorkNegotiating();
      if (_State == State::kRunning && lastState != State::kRunning) {
        co_await _Parent._Notification.get().OnChannelEstablished(
            std::dynamic_pointer_cast<Channel>(shared_from_this()));
      }
      lastState = _State;
      break;
    }
    case State::kRunning: {
      _State = co_await DoWorkRunning();
      if (_State != State::kRunning && lastState == State::kRunning) {
        co_await _Parent._Notification.get().OnChannelClosed(std::dynamic_pointer_cast<Channel>(shared_from_this()));
      }
      lastState = _State;
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
  if (lastState == State::kRunning) {
    co_await _Parent._Notification.get().OnChannelClosed(std::dynamic_pointer_cast<Channel>(shared_from_this()));
  }
  _State = State::kStopping;
}

Omni::Fiber::Coroutine<UdpDynMux::Channel::State> UdpDynMux::Channel::DoWorkNegotiating() {
  auto duration = BackoffTimerDuration(50, std::chrono::milliseconds(1000), std::chrono::milliseconds(2000),
                                       std::chrono::milliseconds(30000));
  while (!_Service.value()._Stop.IsTriggered()) {
    if (_Peer.has_value()) {
      BOOST_LOG_TRIVIAL(info) << std::format("{} negotiating sending initiate Local({}:{}@{})", GetName(), _LocalRxId,
                                             _RemoteRxId, boost::lexical_cast<std::string>(_Peer.value()));
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
          Omni::Fiber::SelectPair(timer.async_wait(_Service.value()._Stop.AsioSlot()()),
                                  Omni::Fiber::AsioApply([](auto ec) { return ec; })));
      if (state.has_value()) {
        if (state.value() != State::kNegotiating) {
          co_return state.value();
        }
      }
    } else if (_PeerResolver) {
      BOOST_LOG_TRIVIAL(info) << GetName() << " resolver " << _PeerResolver->GetName() << " resolving peer";
      auto res = co_await _PeerResolver->Resolve(_Service.value()._Stop);
      if (res.has_value()) {
        BOOST_LOG_TRIVIAL(info) << GetName() << " resolver " << _PeerResolver->GetName()
                                << " resolved peer endpoint: " << res.value();
        _Peer = res.value();
      } else {
        BOOST_LOG_TRIVIAL(warning) << GetName() << " resolver " << _PeerResolver->GetName()
                                   << " resolution failed: " << res.error().message();
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
    if (now - _LastSeen > KeepaliveTimeout) {
      BOOST_LOG_TRIVIAL(warning) << GetName() << " session timeout, resetting to negotiating";
      _Peer = std::nullopt;
      _RemoteRxId = 0;
      co_return State::kNegotiating;
    }

    if (now >= _NextKeepaliveTime) {
      AdjustKeepaliveTimers(now);
      co_await _Parent.SendControlKeepalive(_Peer.value(), _Psk, KeepaliveFlags::kPing);
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
        Omni::Fiber::SelectPair(timer.async_wait(_Service.value()._Stop.AsioSlot()()),
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
  auto now = std::chrono::steady_clock::now();
  if (!PacketUdpDynMux::Validate(packet.Data())) {
    co_return _State;
  }
  auto action =
      PacketParser<std::function<Omni::Fiber::Coroutine<UdpDynMux::Channel::State>()>, PacketUdpDynMux, 0>{
          packet.Data()}(Overload{
          [&](C<EnumChannel::kControlChannel>) {
            return Overload{
                [&](C<MsgType::kInitiate>) {
                  return [&](auto pskSpan, auto rxId, auto peerRxId, auto major, auto minor, auto patch) {
                    UdpDynMux::PskType psk;
                    std::copy(pskSpan.begin(), pskSpan.end(), psk.begin());
                    return [this, now, &peer, psk, rxId, peerRxId, major, minor,
                            patch] -> Omni::Fiber::Coroutine<UdpDynMux::Channel::State> {
                      // Validate version compatibility
                      if (major != kMajorVersion || minor != kMinorVersion) {
                        BOOST_LOG_TRIVIAL(warning)
                            << std::format("{} received incompatible INITIATE: version {}.{}.{} (expected: {}.{}.x)",
                                           GetName(), major, minor, patch, kMajorVersion, kMinorVersion);
                        co_await _Parent.SendControlInitiateFail(peer, psk);
                        co_return State::kStopping;
                      }

                      if (_Peer.has_value()) {
                        BOOST_LOG_TRIVIAL(info)
                            << std::format("{} received initiate: Local({}:{}@{}) Peer({}:{}@{})", GetName(),
                                           _LocalRxId, _RemoteRxId, boost::lexical_cast<std::string>(_Peer.value()),
                                           peerRxId, rxId, boost::lexical_cast<std::string>(peer));
                      } else {
                        BOOST_LOG_TRIVIAL(info) << std::format(
                            "{} received initiate: Local({}:{}@<none>) Peer({}:{}@{})", GetName(), _LocalRxId,
                            _RemoteRxId, peerRxId, rxId, boost::lexical_cast<std::string>(peer));
                      }
                      bool myRxMatches = (peerRxId == _LocalRxId);
                      bool peerRxMatches = (rxId == _RemoteRxId && _Peer == peer);
                      _LastSeen = now;
                      if (!peerRxMatches) {
                        _RemoteRxId = rxId;
                        _Peer = peer; // peer address is strictly updated only on receiving INITIATE
                      }

                      if (!myRxMatches) {
                        co_await _Parent.SendControlInitiate(peer, psk, _LocalRxId, _RemoteRxId);
                      }

                      co_return State::kRunning;
                    };
                  };
                },
                [&](C<MsgType::kInitiateFail>) {
                  return [&](auto psk, auto major, auto minor, auto patch) {
                    return [this, &peer, major, minor, patch] -> Omni::Fiber::Coroutine<UdpDynMux::Channel::State> {
                      BOOST_LOG_TRIVIAL(warning)
                          << std::format("{} received INITIATE_FAIL from peer {} version {}.{}.{}, closing channel",
                                         GetName(), boost::lexical_cast<std::string>(peer), major, minor, patch);
                      co_return State::kStopping;
                    };
                  };
                },
                [&](C<MsgType::kKeepalive>) {
                  return [&](auto psk, auto flag) {
                    return [this, now, &peer, flag] -> Omni::Fiber::Coroutine<UdpDynMux::Channel::State> {
                      if (_Peer == peer) {
                        _LastSeen = now;

                        if (flag & KeepaliveFlags::kPong) {
                          _RoundTripTime =
                              std::chrono::duration_cast<std::chrono::milliseconds>(now - _LastPingSentTime);
                        }

                        if (flag & KeepaliveFlags::kPing) {
                          uint8_t responseFlags = KeepaliveFlags::kPong;
                          if (now > _NextKeepaliveSilentTime) {
                            responseFlags |= KeepaliveFlags::kPing;
                            AdjustKeepaliveTimers(now);
                          }
                          co_await _Parent.SendControlKeepalive(peer, _Psk, responseFlags);
                        }
                      } else {
                        co_await _Parent.SendControlInvalidAddress(peer, _LocalRxId);
                      }
                      co_return _State;
                    };
                  };
                },
                [&](C<MsgType::kInvalidPsk>) {
                  return [&](auto psk) {
                    return [this] -> Omni::Fiber::Coroutine<UdpDynMux::Channel::State> {
                      BOOST_LOG_TRIVIAL(warning) << GetName() << " received INVALID_PSK, restart channel";
                      _Peer = std::nullopt;
                      _RemoteRxId = 0;
                      co_return State::kNegotiating;
                    };
                  };
                },
                [&](C<MsgType::kInvalidAddress>) {
                  return [&](auto channel) {
                    return [this, &peer, channel] -> Omni::Fiber::Coroutine<UdpDynMux::Channel::State> {
                      if (_Peer == peer && _RemoteRxId == channel) {
                        BOOST_LOG_TRIVIAL(warning)
                            << GetName()
                            << " received INVALID_ADDRESS, resetting state to negotiating (without resolving)";
                        _RemoteRxId = 0;
                        co_return State::kNegotiating;
                      }
                      co_return _State;
                    };
                  };
                },
                [&](C<MsgType::kInvalidChannel>) {
                  return [&](auto channel) {
                    return [this, &peer, channel] -> Omni::Fiber::Coroutine<UdpDynMux::Channel::State> {
                      if (_Peer == peer && _RemoteRxId == channel) {
                        BOOST_LOG_TRIVIAL(warning)
                            << GetName() << " received INVALID_CHANNEL, resetting state to negotiating";
                        _Peer = std::nullopt;
                        _RemoteRxId = 0;
                        co_return State::kNegotiating;
                      }
                      co_return _State;
                    };
                  };
                },
                [&](MsgType value) {
                  return [this, value] -> Omni::Fiber::Coroutine<UdpDynMux::Channel::State> {
                    BOOST_LOG_TRIVIAL(warning)
                        << GetName() << " received Invalid msg type: " << std::to_underlying(value) << " from peer "
                        << boost::lexical_cast<std::string>(_Peer.value());
                    co_return _State;
                  };
                },
            };
          },
          [&](EnumChannel value) {
            return [this, value] -> Omni::Fiber::Coroutine<UdpDynMux::Channel::State> {
              assert(false && "Should not receive invalid channel value in HandleControlMessage");
              co_return _State;
            };
          },
      });

  if (auto func = action.Value()) {
    co_return co_await func();
  }
  co_return _State;
}

// ==================== UdpDynMux ====================

UdpDynMux::UdpDynMux(boost::asio::any_io_executor executor, ChannelNotification& notification)
    : UdpDynMux(executor, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v6(), 0), notification) {}

UdpDynMux::UdpDynMux(boost::asio::any_io_executor executor, boost::asio::ip::udp::endpoint bind,
                     ChannelNotification& notification)
    : _Notification(notification), _Socket(executor), _Local(bind) {
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
  _ReadLoopFiber = currentFiber.Spawn(GetName() + " ReadLoop", [this]() -> Omni::Fiber::Coroutine<void> {
    co_await ReadLoop();
    co_return;
  });

  bool stopped = false;
  while (!stopped) {
    auto [stopResult, rpcResult] = co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(), [] {}),
        Omni::Fiber::SelectPair(_ChannelRpc.GetServiceAwaitor(), Omni::Fiber::RemoteCall::HandleRequest));
    if (stopResult) {
      stopped = true;
    }
    if (rpcResult.has_value() && !rpcResult.value()) {
      stopped = true;
    }
  }
}

Omni::Fiber::Coroutine<ErrorCode> UdpDynMux::DoGracefulStop() {
  co_await _ChannelRpc.Close();
  for (auto& [psk, channel] : std::exchange(_Channels, {})) {
    co_await channel->Stop();
    co_await channel->WaitService();
  }
  _RxIdToChannel.clear();
  if (_ReadLoopFiber) {
    co_await (co_await Omni::Fiber::GetCurrentFiber()).Join(_ReadLoopFiber);
    _ReadLoopFiber.reset();
  }
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
        auto now = std::chrono::steady_clock::now();
        auto channel = std::make_shared<Channel>(udp, psk, udp.AllocateUniqueRxId(), resolver);
        channel->_LastSeen = now;
        channel->_NextKeepaliveTime = now + UdpDynMux::Channel::MinKeepaliveInterval;

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
  while (!_Service.value()._Stop.IsTriggered()) {
    Packet packet;
    boost::asio::ip::udp::endpoint peer;

    auto [err, bytes_transferred] = co_await _Socket.async_receive_from(boost::asio::mutable_buffer(packet), peer,
                                                                        _Service.value()._Stop.AsioSlot()());
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

    uint16_t channelId = 0;
    if (packet._Length >= 2) {
      channelId = (packet._Data[packet._Offset] << 8) | packet._Data[packet._Offset + 1];
    }
    if (channelId == 0) {
      // Control packet
      if (PacketUdpDynMux::Validate(packet.Data())) {
        auto lookup =
            PacketParser<std::variant<UdpDynMux::PskType, uint16_t, std::monostate>, PacketUdpDynMux, 0>{packet.Data()}(
                Overload{
                    [&](C<EnumChannel::kControlChannel>) {
                      return Overload{
                          [&](C<MsgType::kInitiate>) {
                            return [&](auto psk, auto rxId, auto peerRxId, auto major, auto minor, auto patch) {
                              UdpDynMux::PskType pskArr;
                              std::copy(psk.begin(), psk.end(), pskArr.begin());
                              return std::variant<UdpDynMux::PskType, uint16_t, std::monostate>{pskArr};
                            };
                          },
                          [&](C<MsgType::kInitiateFail>) {
                            return [&](auto psk, auto major, auto minor, auto patch) {
                              UdpDynMux::PskType pskArr;
                              std::copy(psk.begin(), psk.end(), pskArr.begin());
                              return std::variant<UdpDynMux::PskType, uint16_t, std::monostate>{pskArr};
                            };
                          },
                          [&](C<MsgType::kKeepalive>) {
                            return [&](auto psk, auto flag) {
                              UdpDynMux::PskType pskArr;
                              std::copy(psk.begin(), psk.end(), pskArr.begin());
                              return std::variant<UdpDynMux::PskType, uint16_t, std::monostate>{pskArr};
                            };
                          },
                          [&](C<MsgType::kInvalidPsk>) {
                            return [&](auto psk) {
                              UdpDynMux::PskType pskArr;
                              std::copy(psk.begin(), psk.end(), pskArr.begin());
                              return std::variant<UdpDynMux::PskType, uint16_t, std::monostate>{pskArr};
                            };
                          },
                          [&](C<MsgType::kInvalidAddress>) {
                            return [&](auto channel) {
                              return std::variant<UdpDynMux::PskType, uint16_t, std::monostate>{channel};
                            };
                          },
                          [&](C<MsgType::kInvalidChannel>) {
                            return [&](auto channel) {
                              return std::variant<UdpDynMux::PskType, uint16_t, std::monostate>{channel};
                            };
                          },
                          [&](MsgType value) {
                            return std::variant<UdpDynMux::PskType, uint16_t, std::monostate>{std::monostate{}};
                          },
                      };
                    },
                    [&](EnumChannel value) {
                      return std::variant<UdpDynMux::PskType, uint16_t, std::monostate>{std::monostate{}};
                    },
                })
                .Value();

        co_await std::visit(
            Overload{
                [&](const UdpDynMux::PskType& psk) -> Omni::Fiber::Coroutine<void> {
                  auto it = _Channels.find(psk);
                  if (it != _Channels.end()) {
                    co_await it->second->_ControlPacket.GetProducer().Put(std::make_tuple(peer, std::move(packet)));
                  }
                },
                [&](uint16_t rxId) -> Omni::Fiber::Coroutine<void> {
                  for (auto& [psk, channel] : _Channels) {
                    if (channel->_RemoteRxId == rxId && channel->_Peer == peer) {
                      co_await channel->_ControlPacket.GetProducer().Put(std::make_tuple(peer, std::move(packet)));
                    }
                  }
                },
                [&](std::monostate) -> Omni::Fiber::Coroutine<void> { co_return; },
            },
            lookup);
      }
    } else {
      if (auto it = _RxIdToChannel.find(channelId); it != _RxIdToChannel.end()) {
        auto channel = it->second;
        if (channel->_State == Channel::State::kRunning && channel->_Peer == peer) {
          packet.PopFront(2);
          co_await channel->_DataPacket.GetProducer().Put(std::move(packet));
        } else if (channel->_Peer.has_value() && channel->_Peer.value() != peer) {
          co_await SendControlInvalidAddress(peer, channelId);
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
  auto [err, bytes_transferred] = co_await _Socket.async_send_to(boost::asio::const_buffer(p), peer, c.AsioSlot()());
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

Omni::Fiber::Coroutine<void> UdpDynMux::SendControlInitiate(const boost::asio::ip::udp::endpoint& peer,
                                                            const UdpDynMux::PskType& psk, uint16_t rxId,
                                                            uint16_t peerRxId) {
  constexpr size_t kSize = 27; // 2 + 1 + 16 + 2 + 2 + 1 + 1 + 2
  auto buf = std::make_shared<std::array<uint8_t, kSize>>();
  PacketBuilder<PacketUdpDynMux, 0> builder(*buf);
  builder(ToC<EnumChannel::kControlChannel>())(ToC<MsgType::kInitiate>())(
      std::span<const uint8_t, 16>(psk), rxId, peerRxId, kMajorVersion, kMinorVersion, kPatchVersion);
  co_await _Socket.async_send_to(boost::asio::buffer(*buf), peer, Omni::Fiber::AsioUseFiber);
}

Omni::Fiber::Coroutine<void> UdpDynMux::SendControlInitiateFail(const boost::asio::ip::udp::endpoint& peer,
                                                                const UdpDynMux::PskType& psk) {
  constexpr size_t kSize = 23; // 2 + 1 + 16 + 1 + 1 + 2
  auto buf = std::make_shared<std::array<uint8_t, kSize>>();
  PacketBuilder<PacketUdpDynMux, 0> builder(*buf);
  builder(ToC<EnumChannel::kControlChannel>())(ToC<MsgType::kInitiateFail>())(
      std::span<const uint8_t, 16>(psk), kMajorVersion, kMinorVersion, kPatchVersion);
  co_await _Socket.async_send_to(boost::asio::buffer(*buf), peer, Omni::Fiber::AsioUseFiber);
}

Omni::Fiber::Coroutine<void> UdpDynMux::SendControlKeepalive(const boost::asio::ip::udp::endpoint& peer,
                                                             const UdpDynMux::PskType& psk, uint8_t flags) {
  constexpr size_t kSize = 20; // 2 + 1 + 16 + 1
  auto buf = std::make_shared<std::array<uint8_t, kSize>>();
  PacketBuilder<PacketUdpDynMux, 0> builder(*buf);
  builder(ToC<EnumChannel::kControlChannel>())(ToC<MsgType::kKeepalive>())(std::span<const uint8_t, 16>(psk), flags);
  co_await _Socket.async_send_to(boost::asio::buffer(*buf), peer, Omni::Fiber::AsioUseFiber);
}

Omni::Fiber::Coroutine<void> UdpDynMux::SendControlInvalidPsk(const boost::asio::ip::udp::endpoint& peer,
                                                              const UdpDynMux::PskType& psk) {
  if (!CheckRateLimit(peer)) {
    co_return;
  }
  constexpr size_t kSize = 19; // 2 + 1 + 16
  auto buf = std::make_shared<std::array<uint8_t, kSize>>();
  PacketBuilder<PacketUdpDynMux, 0> builder(*buf);
  builder(ToC<EnumChannel::kControlChannel>())(ToC<MsgType::kInvalidPsk>())(std::span<const uint8_t, 16>(psk));
  co_await _Socket.async_send_to(boost::asio::buffer(*buf), peer, Omni::Fiber::AsioUseFiber);
}

Omni::Fiber::Coroutine<void> UdpDynMux::SendControlInvalidChannel(const boost::asio::ip::udp::endpoint& peer,
                                                                  uint16_t channelId) {
  if (!CheckRateLimit(peer)) {
    co_return;
  }
  constexpr size_t kSize = 5; // 2 + 1 + 2
  auto buf = std::make_shared<std::array<uint8_t, kSize>>();
  PacketBuilder<PacketUdpDynMux, 0> builder(*buf);
  builder(ToC<EnumChannel::kControlChannel>())(ToC<MsgType::kInvalidChannel>())(channelId);
  co_await _Socket.async_send_to(boost::asio::buffer(*buf), peer, Omni::Fiber::AsioUseFiber);
}

Omni::Fiber::Coroutine<void> UdpDynMux::SendControlInvalidAddress(const boost::asio::ip::udp::endpoint& peer,
                                                                  uint16_t channelId) {
  if (!CheckRateLimit(peer)) {
    co_return;
  }
  constexpr size_t kSize = 5; // 2 + 1 + 2
  auto buf = std::make_shared<std::array<uint8_t, kSize>>();
  PacketBuilder<PacketUdpDynMux, 0> builder(*buf);
  builder(ToC<EnumChannel::kControlChannel>())(ToC<MsgType::kInvalidAddress>())(channelId);
  co_await _Socket.async_send_to(boost::asio::buffer(*buf), peer, Omni::Fiber::AsioUseFiber);
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
