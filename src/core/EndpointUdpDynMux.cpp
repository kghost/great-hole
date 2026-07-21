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
#include "GetCurrentOmniFiber.hpp"
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

auto UdpDynMux::Channel::GetName() const -> std::string {
  return std::format("UdpDynMuxChannel:{}:[psk_hash:{:02x}]", _Parent.GetName(), _Psk[0]);
}

auto UdpDynMux::Channel::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> { co_return ErrorCode{}; }

auto UdpDynMux::Channel::DoWork() -> Omni::Fiber::Coroutine<void> {
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

auto UdpDynMux::Channel::DoWorkNegotiating() -> Omni::Fiber::Coroutine<UdpDynMux::Channel::State> {
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
                                  [&]() -> ErrorCode { return Error(AppErrorCategory::kOperationAborted); }),
          Omni::Fiber::SelectPair(_ControlPacket.GetConsumer(),
                                  [&](auto info) -> Omni::Fiber::Coroutine<UdpDynMux::Channel::State> {
                                    assert(info.has_value() && "Pipe should never ends");
                                    auto [peer, packet] = std::move(info.value());
                                    // FIXME: there are gcc bugs which causes packet object leaks
                                    co_return co_await HandleControlPacket(peer, packet);
                                  }),
          Omni::Fiber::SelectPair(timer.async_wait(_Service.value()._Stop.AsioSlot()()),
                                  Omni::Fiber::AsioApply([](auto ec) -> auto { return ec; })));
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
                                  [&]() -> ErrorCode { return Error(AppErrorCategory::kOperationAborted); }),
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

auto UdpDynMux::Channel::DoWorkRunning() -> Omni::Fiber::Coroutine<UdpDynMux::Channel::State> {
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
                                [&]() -> ErrorCode { return Error(AppErrorCategory::kOperationAborted); }),
        Omni::Fiber::SelectPair(_ControlPacket.GetConsumer(),
                                [&](auto info) -> Omni::Fiber::Coroutine<UdpDynMux::Channel::State> {
                                  assert(info.has_value() && "Pipe should never ends");
                                  auto [peer, packet] = std::move(info.value());
                                  // FIXME: there are gcc bugs which causes packet object leaks
                                  co_return co_await HandleControlPacket(peer, packet);
                                }),
        Omni::Fiber::SelectPair(timer.async_wait(_Service.value()._Stop.AsioSlot()()),
                                Omni::Fiber::AsioApply([](auto ec) -> auto { return ec; })));
    if (state.has_value() && state.value() != State::kRunning) {
      co_return state.value();
    }
  }

  co_return State::kStopping;
}

auto UdpDynMux::Channel::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> {
  co_await _PipielineUsageCounter.WaitAll();
  _DataPacket.GetConsumer().DiscardAndClose();
  _ControlPacket.GetConsumer().DiscardAndClose();
  co_return ErrorCode{};
}

auto UdpDynMux::Channel::Read(Packet& p, Cancel& c) -> Omni::Fiber::Coroutine<ErrorCode> {
  auto [stopped, err] =
      co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(c.GetFiberCancelEvent(), [] -> void {}),
                                   Omni::Fiber::SelectPair(_DataPacket.GetConsumer(), [&](auto data) -> ErrorCode {
                                     if (data.has_value()) {
                                       _LastSeen = std::chrono::steady_clock::now();
                                       p = std::move(data.value());
                                       return ErrorCode{};
                                     } else {
                                       p._Length = 0;
                                       return Error(AppErrorCategory::kEndOfStream);
                                     }
                                   }));
  if (err.has_value()) {
    co_return err.value();
  }
  if (stopped) {
    co_return Error(AppErrorCategory::kOperationAborted);
  }
  assert(false && "should not reach here");
  co_return ErrorCode{};
}

auto UdpDynMux::Channel::Write(Packet& p, Cancel& c) -> Omni::Fiber::Coroutine<ErrorCode> {
  if (c.IsTriggered() || ServiceBase::_State != ServiceBase::State::kRunning) {
    co_return Error(AppErrorCategory::kOperationAborted);
  }

  if (_State != State::kRunning) {
    co_return Error(AppMinorErrorCategory::kInvalidPacketSession);
  }

  if (!_Peer.has_value() || _RemoteRxId == 0) {
    co_return Error(AppMinorErrorCategory::kInvalidPacketSession);
  }

  if (p._Offset < 2) {
    co_return Error(AppErrorCategory::kInvalidPacketReserved);
  }

  p.PushFront(_RemoteRxId);

  co_return co_await _Parent.WriteTo(_Peer.value(), p, c);
}

auto UdpDynMux::Channel::HandleControlPacket(boost::asio::ip::udp::endpoint peer, Packet& packet)
    -> Omni::Fiber::Coroutine<UdpDynMux::Channel::State> {
  auto now = std::chrono::steady_clock::now();
  if (!PacketUdpDynMux::Validate(packet.Data())) {
    co_return _State;
  }
  auto action =
      PacketParser<std::function<Omni::Fiber::Coroutine<UdpDynMux::Channel::State>()>, PacketUdpDynMux, 0>{
          packet.Data()}(Overload{
          [&](C<EnumChannel::kControlChannel>) -> auto {
            return Overload{
                [&](C<MsgType::kInitiate>) -> auto {
                  return [&](auto pskSpan, auto rxId, auto peerRxId, auto major, auto minor, auto patch) -> auto {
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
                [&](C<MsgType::kInitiateFail>) -> auto {
                  return [&](auto psk, auto major, auto minor, auto patch) -> auto {
                    return [this, &peer, major, minor, patch] -> Omni::Fiber::Coroutine<UdpDynMux::Channel::State> {
                      BOOST_LOG_TRIVIAL(warning)
                          << std::format("{} received INITIATE_FAIL from peer {} version {}.{}.{}, closing channel",
                                         GetName(), boost::lexical_cast<std::string>(peer), major, minor, patch);
                      co_return State::kStopping;
                    };
                  };
                },
                [&](C<MsgType::kKeepalive>) -> auto {
                  return [&](auto psk, auto flag) -> auto {
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
                [&](C<MsgType::kInvalidPsk>) -> auto {
                  return [&](auto psk) -> auto {
                    return [this] -> Omni::Fiber::Coroutine<UdpDynMux::Channel::State> {
                      BOOST_LOG_TRIVIAL(warning) << GetName() << " received INVALID_PSK, restart channel";
                      _Peer = std::nullopt;
                      _RemoteRxId = 0;
                      co_return State::kNegotiating;
                    };
                  };
                },
                [&](C<MsgType::kInvalidAddress>) -> auto {
                  return [&](auto channel) -> auto {
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
                [&](C<MsgType::kInvalidChannel>) -> auto {
                  return [&](auto channel) -> auto {
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
                [&](MsgType value) -> auto {
                  return [this, value] -> Omni::Fiber::Coroutine<UdpDynMux::Channel::State> {
                    BOOST_LOG_TRIVIAL(warning)
                        << GetName() << " received Invalid msg type: " << std::to_underlying(value) << " from peer "
                        << boost::lexical_cast<std::string>(_Peer.value());
                    co_return _State;
                  };
                },
            };
          },
          [&](EnumChannel _) -> auto {
            return [this] -> Omni::Fiber::Coroutine<UdpDynMux::Channel::State> {
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

auto UdpDynMux::GetName() const -> std::string { return "UdpDynMux:" + boost::lexical_cast<std::string>(_Local); }

auto UdpDynMux::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> {
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
    _ChannelRpc.DiscardAndClose();
    co_return ec;
  }
  co_return ErrorCode{};
}

auto UdpDynMux::DoWork() -> Omni::Fiber::Coroutine<void> {
  auto& currentFiber = co_await Omni::Fiber::GetCurrentOmniFiber();
  _ReadLoopFiber = currentFiber.Spawn(GetName() + " ReadLoop", [this]() -> Omni::Fiber::Coroutine<void> {
    co_await ReadLoop();
    co_return;
  });

  bool stopped = false;
  while (!stopped) {
    auto [stopResult, rpcResult] = co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(), [] -> void {}),
        Omni::Fiber::SelectPair(_ChannelRpc.GetServiceAwaitor(), Omni::Fiber::RemoteCall::HandleRequest));
    if (stopResult) {
      stopped = true;
    }
    if (rpcResult.has_value() && !rpcResult.value()) {
      stopped = true;
    }
  }
}

auto UdpDynMux::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> {
  _ChannelRpc.DiscardAndClose();
  for (auto& [psk, channel] : std::exchange(_Channels, {})) {
    co_await channel->Stop();
  }
  _RxIdToChannel.clear();
  if (_ReadLoopFiber) {
    co_await (co_await Omni::Fiber::GetCurrentOmniFiber()).Join(_ReadLoopFiber);
    _ReadLoopFiber.reset();
  }
  _Socket.close();
  co_return ErrorCode{};
}

auto UdpDynMux::CreateChannel(const UdpDynMux::PskType& psk)
    -> Omni::Fiber::Coroutine<std::shared_ptr<UdpDynMux::Channel>> {
  co_return co_await CreateChannel(psk, nullptr);
}

auto UdpDynMux::CreateChannel(const UdpDynMux::PskType& psk, std::shared_ptr<ResolverEndpoint> resolver)
    -> Omni::Fiber::Coroutine<std::shared_ptr<UdpDynMux::Channel>> {
  auto reply =
      co_await _ChannelRpc.Call([&udp = *this, psk, resolver] -> Omni::Fiber::Coroutine<std::shared_ptr<Channel>> {
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
          co_await channel->Stop();
          co_return nullptr;
        }

        co_return channel;
      });
  assert(reply.has_value());
  co_return reply.value();
}

auto UdpDynMux::RemoveChannel(const UdpDynMux::PskType& psk) -> Omni::Fiber::Coroutine<void> {
  auto result = co_await _ChannelRpc.Call([this, psk]() -> Omni::Fiber::Coroutine<void> {
    auto it = _Channels.find(psk);
    if (it != _Channels.end()) {
      auto channel = std::move(it->second);
      _RxIdToChannel.erase(channel->_LocalRxId);
      _Channels.erase(it);
      co_await channel->Stop();
    }
  });
  if (!result.has_value()) {
    BOOST_LOG_TRIVIAL(error) << GetName() << " remove channel failed";
  }
}

auto UdpDynMux::ReadLoop() -> Omni::Fiber::Coroutine<void> {
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
                    [&](C<EnumChannel::kControlChannel>) -> auto {
                      return Overload{
                          [&](C<MsgType::kInitiate>) -> auto {
                            return [&](auto psk, auto rxId, auto peerRxId, auto major, auto minor, auto patch) -> auto {
                              UdpDynMux::PskType pskArr;
                              std::copy(psk.begin(), psk.end(), pskArr.begin());
                              return std::variant<UdpDynMux::PskType, uint16_t, std::monostate>{pskArr};
                            };
                          },
                          [&](C<MsgType::kInitiateFail>) -> auto {
                            return [&](auto psk, auto major, auto minor, auto patch) -> auto {
                              UdpDynMux::PskType pskArr;
                              std::copy(psk.begin(), psk.end(), pskArr.begin());
                              return std::variant<UdpDynMux::PskType, uint16_t, std::monostate>{pskArr};
                            };
                          },
                          [&](C<MsgType::kKeepalive>) -> auto {
                            return [&](auto psk, auto flag) -> auto {
                              UdpDynMux::PskType pskArr;
                              std::copy(psk.begin(), psk.end(), pskArr.begin());
                              return std::variant<UdpDynMux::PskType, uint16_t, std::monostate>{pskArr};
                            };
                          },
                          [&](C<MsgType::kInvalidPsk>) -> auto {
                            return [&](auto psk) -> auto {
                              UdpDynMux::PskType pskArr;
                              std::copy(psk.begin(), psk.end(), pskArr.begin());
                              return std::variant<UdpDynMux::PskType, uint16_t, std::monostate>{pskArr};
                            };
                          },
                          [&](C<MsgType::kInvalidAddress>) -> auto {
                            return [&](auto channel) -> auto {
                              return std::variant<UdpDynMux::PskType, uint16_t, std::monostate>{channel};
                            };
                          },
                          [&](C<MsgType::kInvalidChannel>) -> auto {
                            return [&](auto channel) -> auto {
                              return std::variant<UdpDynMux::PskType, uint16_t, std::monostate>{channel};
                            };
                          },
                          [&](MsgType value) -> std::variant<UdpDynMux::PskType, uint16_t, std::monostate> {
                            return std::variant<UdpDynMux::PskType, uint16_t, std::monostate>{std::monostate{}};
                          },
                      };
                    },
                    [&](EnumChannel value) -> std::variant<UdpDynMux::PskType, uint16_t, std::monostate> {
                      return std::variant<UdpDynMux::PskType, uint16_t, std::monostate>{std::monostate{}};
                    },
                })
                .Value();

        co_await std::visit(Overload{
                                [&](const UdpDynMux::PskType& psk) -> Omni::Fiber::Coroutine<void> {
                                  auto it = _Channels.find(psk);
                                  if (it != _Channels.end()) {
                                    auto result = co_await it->second->_ControlPacket.GetProducer().Put(
                                        std::make_tuple(peer, std::move(packet)));
                                    if (!result.has_value()) {
                                      BOOST_LOG_TRIVIAL(error) << GetName() << " control packet pipe failed";
                                    }
                                  }
                                },
                                [&](uint16_t rxId) -> Omni::Fiber::Coroutine<void> {
                                  for (auto& [psk, channel] : _Channels) {
                                    if (channel->_RemoteRxId == rxId && channel->_Peer == peer) {
                                      auto result = co_await channel->_ControlPacket.GetProducer().Put(
                                          std::make_tuple(peer, std::move(packet)));
                                      if (!result.has_value()) {
                                        BOOST_LOG_TRIVIAL(error) << GetName() << " control packet pipe failed";
                                      }
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
          packet.PopFront<FieldChannel::Size>();
          auto result = co_await channel->_DataPacket.GetProducer().Put(std::move(packet));
          if (!result.has_value()) {
            BOOST_LOG_TRIVIAL(error) << GetName() << " data packet pipe failed";
          }
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

auto UdpDynMux::WriteTo(boost::asio::ip::udp::endpoint peer, Packet& p, Cancel& c)
    -> Omni::Fiber::Coroutine<ErrorCode> {
  auto [err, bytes_transferred] = co_await _Socket.async_send_to(boost::asio::const_buffer(p), peer, c.AsioSlot()());
  assert(err || bytes_transferred == p._Length);
  co_return err;
}

auto UdpDynMux::CheckRateLimit(const boost::asio::ip::udp::endpoint& peer) -> bool {
  auto now = std::chrono::steady_clock::now();
  std::erase_if(_LastErrorSent,
                [&](const auto& entry) -> auto { return now - entry.second > std::chrono::seconds(10); });
  if (auto it = _LastErrorSent.find(peer); it != _LastErrorSent.end() && now - it->second < std::chrono::seconds(1)) {
    return false;
  }
  _LastErrorSent[peer] = now;
  return true;
}

auto UdpDynMux::SendControlInitiate(const boost::asio::ip::udp::endpoint& peer, const UdpDynMux::PskType& psk,
                                    uint16_t rxId, uint16_t peerRxId) -> Omni::Fiber::Coroutine<void> {
  constexpr size_t kSize = 27; // 2 + 1 + 16 + 2 + 2 + 1 + 1 + 2
  auto buf = std::make_shared<std::array<uint8_t, kSize>>();
  PacketBuilder<PacketUdpDynMux, 0> builder(*buf);
  builder(ToC<EnumChannel::kControlChannel>())(ToC<MsgType::kInitiate>())(
      std::span<const uint8_t, 16>(psk), rxId, peerRxId, kMajorVersion, kMinorVersion, kPatchVersion);
  co_await _Socket.async_send_to(boost::asio::buffer(*buf), peer, Omni::Fiber::AsioUseFiber);
}

auto UdpDynMux::SendControlInitiateFail(const boost::asio::ip::udp::endpoint& peer, const UdpDynMux::PskType& psk)
    -> Omni::Fiber::Coroutine<void> {
  constexpr size_t kSize = 23; // 2 + 1 + 16 + 1 + 1 + 2
  auto buf = std::make_shared<std::array<uint8_t, kSize>>();
  PacketBuilder<PacketUdpDynMux, 0> builder(*buf);
  builder(ToC<EnumChannel::kControlChannel>())(ToC<MsgType::kInitiateFail>())(
      std::span<const uint8_t, 16>(psk), kMajorVersion, kMinorVersion, kPatchVersion);
  co_await _Socket.async_send_to(boost::asio::buffer(*buf), peer, Omni::Fiber::AsioUseFiber);
}

auto UdpDynMux::SendControlKeepalive(const boost::asio::ip::udp::endpoint& peer, const UdpDynMux::PskType& psk,
                                     uint8_t flags) -> Omni::Fiber::Coroutine<void> {
  constexpr size_t kSize = 20; // 2 + 1 + 16 + 1
  auto buf = std::make_shared<std::array<uint8_t, kSize>>();
  PacketBuilder<PacketUdpDynMux, 0> builder(*buf);
  builder(ToC<EnumChannel::kControlChannel>())(ToC<MsgType::kKeepalive>())(std::span<const uint8_t, 16>(psk), flags);
  co_await _Socket.async_send_to(boost::asio::buffer(*buf), peer, Omni::Fiber::AsioUseFiber);
}

auto UdpDynMux::SendControlInvalidPsk(const boost::asio::ip::udp::endpoint& peer, const UdpDynMux::PskType& psk)
    -> Omni::Fiber::Coroutine<void> {
  if (!CheckRateLimit(peer)) {
    co_return;
  }
  constexpr size_t kSize = 19; // 2 + 1 + 16
  auto buf = std::make_shared<std::array<uint8_t, kSize>>();
  PacketBuilder<PacketUdpDynMux, 0> builder(*buf);
  builder(ToC<EnumChannel::kControlChannel>())(ToC<MsgType::kInvalidPsk>())(std::span<const uint8_t, 16>(psk));
  co_await _Socket.async_send_to(boost::asio::buffer(*buf), peer, Omni::Fiber::AsioUseFiber);
}

auto UdpDynMux::SendControlInvalidChannel(const boost::asio::ip::udp::endpoint& peer, uint16_t channelId)
    -> Omni::Fiber::Coroutine<void> {
  if (!CheckRateLimit(peer)) {
    co_return;
  }
  constexpr size_t kSize = 5; // 2 + 1 + 2
  auto buf = std::make_shared<std::array<uint8_t, kSize>>();
  PacketBuilder<PacketUdpDynMux, 0> builder(*buf);
  builder(ToC<EnumChannel::kControlChannel>())(ToC<MsgType::kInvalidChannel>())(channelId);
  co_await _Socket.async_send_to(boost::asio::buffer(*buf), peer, Omni::Fiber::AsioUseFiber);
}

auto UdpDynMux::SendControlInvalidAddress(const boost::asio::ip::udp::endpoint& peer, uint16_t channelId)
    -> Omni::Fiber::Coroutine<void> {
  if (!CheckRateLimit(peer)) {
    co_return;
  }
  constexpr size_t kSize = 5; // 2 + 1 + 2
  auto buf = std::make_shared<std::array<uint8_t, kSize>>();
  PacketBuilder<PacketUdpDynMux, 0> builder(*buf);
  builder(ToC<EnumChannel::kControlChannel>())(ToC<MsgType::kInvalidAddress>())(channelId);
  co_await _Socket.async_send_to(boost::asio::buffer(*buf), peer, Omni::Fiber::AsioUseFiber);
}

auto UdpDynMux::AllocateUniqueRxId() -> uint16_t {
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
