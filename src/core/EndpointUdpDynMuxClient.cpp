#include "EndpointUdpDynMuxClient.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <expected>
#include <memory>
#include <random>
#include <utility>

#include <boost/asio/buffer.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "EndpointUdpDynMuxProtocol.hpp"
#include "ErrorCode.hpp"
#include "GetCurrentFiber.hpp"
#include "Select.hpp"

namespace gh {

UdpDynMuxClient::UdpDynMuxClient(boost::asio::io_context& ioContext, boost::asio::ip::udp::endpoint peer)
    : _Socket(ioContext), _Peer(peer), _Local(boost::asio::ip::udp::v6(), 0) {
  std::random_device rd;
  _Prng.seed(rd());
}

UdpDynMuxClient::UdpDynMuxClient(boost::asio::io_context& ioContext, boost::asio::ip::udp::endpoint peer,
                                 boost::asio::ip::udp::endpoint local)
    : _Socket(ioContext), _Peer(peer), _Local(local) {
  std::random_device rd;
  _Prng.seed(rd());
}

UdpDynMuxClient::~UdpDynMuxClient() {}

std::string UdpDynMuxClient::GetName() const { return "UdpDynMuxClient:" + std::to_string(_AssignedId); }

Omni::Fiber::Coroutine<ErrorCode> UdpDynMuxClient::DoStart() {
  try {
    _Socket.open(boost::asio::ip::udp::v6());
    _Socket.set_option(boost::asio::ip::v6_only(false));
    _Socket.bind(_Local);
    BOOST_LOG_TRIVIAL(info) << "UdpDynMuxClient(" << this << ") bound at " << _Socket.local_endpoint();
  } catch (const SystemError& e) {
    BOOST_LOG_TRIVIAL(info) << "UdpDynMuxClient(" << this << ") start failed on bind: " << e.what();
    co_return e.code();
  }

  auto [err] = co_await _Socket.async_connect(
      _Peer, boost::asio::bind_cancellation_slot(_Stop.AsioSlot().Slot(), Omni::Fiber::AsioUseFiber));
  if (err) {
    BOOST_LOG_TRIVIAL(info) << "UdpDynMuxClient(" << this << ") connect failed: " << err.message();
    co_return err;
  }

  _State = kNegotiating;
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> UdpDynMuxClient::DoGracefulStop() {
  co_await _PipielineUsageCounter.WaitAll();
  _Socket.close();
  co_await (co_await Omni::Fiber::GetCurrentFiber()).WaitAll();
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<void> UdpDynMuxClient::DoWork() {
  auto& currentFiber = co_await Omni::Fiber::GetCurrentFiber();
  currentFiber.Spawn("UdpDynMuxClient ReadLoop:" + boost::lexical_cast<std::string>(LocalEndpoint()) + "@" +
                         std::to_string(reinterpret_cast<uintptr_t>(this)),
                     [this]() -> Omni::Fiber::Coroutine<void> {
                       co_await ReadLoop();
                       co_return;
                     });

  currentFiber.Spawn("UdpDynMuxClient ClientLoop:" + boost::lexical_cast<std::string>(LocalEndpoint()) + "@" +
                         std::to_string(reinterpret_cast<uintptr_t>(this)),
                     [this]() -> Omni::Fiber::Coroutine<void> {
                       co_await ClientLoop();
                       co_return;
                     });

  co_await _Stop.GetFiberCancelEvent();
}

Omni::Fiber::Coroutine<ErrorCode> UdpDynMuxClient::Read(Packet& p, Cancel& c) {
  bool stopped = false;
  std::optional<ErrorCode> err;
  co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(c.GetFiberCancelEvent(), [&]() { stopped = true; }),
                               Omni::Fiber::SelectPair(_DataPipe.GetConsumer(), [&](auto data) {
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

Omni::Fiber::Coroutine<ErrorCode> UdpDynMuxClient::Write(Packet& p, Cancel& c) {
  if (c.IsTriggered()) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }

  while (_State != kRunning && !_Stop.IsTriggered() && !c.IsTriggered()) {
    boost::asio::steady_timer waitTimer(_Socket.get_executor());
    waitTimer.expires_after(std::chrono::milliseconds(100));
    co_await waitTimer.async_wait(boost::asio::bind_cancellation_slot(c.AsioSlot().Slot(), Omni::Fiber::AsioUseFiber));
  }

  if (c.IsTriggered() || _Stop.IsTriggered()) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }

  if (_State == kError) {
    co_return ErrorCode{AppErrorCategory::kIncorrectState, kAppError};
  }

  if (p._Offset < 2) {
    co_return ErrorCode{AppErrorCategory::kInvalidPacketReserved, kAppError};
  }

  p._Offset -= 2;
  p._Length += 2;
  UdpDynMux::WriteUint16Be(p._Data.data() + p._Offset, _AssignedId);

  auto [err, bytes_transferred] =
      co_await _Socket.async_send(boost::asio::const_buffer(p),
                                  boost::asio::bind_cancellation_slot(c.AsioSlot().Slot(), Omni::Fiber::AsioUseFiber));
  assert(err || bytes_transferred == p._Length);
  co_return err;
}

Omni::Fiber::Coroutine<void> UdpDynMuxClient::ReadLoop() {
  auto slotTracker = _Stop.AsioSlot();
  while (!_Stop.IsTriggered()) {
    Packet p;
    auto [err, bytes_transferred] = co_await _Socket.async_receive(
        boost::asio::mutable_buffer(p),
        boost::asio::bind_cancellation_slot(slotTracker.Slot(), Omni::Fiber::AsioUseFiber));
    if (err) {
      if (err == boost::asio::error::operation_aborted) {
        BOOST_LOG_TRIVIAL(info) << "UdpDynMuxClient(" << this << ") read loop cancelled";
      } else {
        BOOST_LOG_TRIVIAL(error) << "UdpDynMuxClient(" << this << ") read error: " << err.message();
        co_await _DataPipe.GetProducer().Put(std::unexpected(err));
      }
      break;
    }

    if (bytes_transferred < 2) {
      BOOST_LOG_TRIVIAL(info) << "UdpDynMuxClient(" << this << ") ignored empty/short packet";
      continue;
    }

    uint16_t channelId = UdpDynMux::ReadUint16Be(p._Data.data() + p._Offset);
    if (channelId == 0) {
      co_await HandleControlPacket(p._Data.data() + p._Offset, bytes_transferred);
    } else {
      if (_State == kRunning && channelId == _AssignedId) {
        p._Offset += 2;
        p._Length = bytes_transferred - 2;
        co_await _DataPipe.GetProducer().Put(std::move(p));
      } else {
        BOOST_LOG_TRIVIAL(debug) << "UdpDynMuxClient(" << this
                                 << ") ignored data packet for unknown channel ID: " << channelId;
      }
    }
  }
}

Omni::Fiber::Coroutine<void> UdpDynMuxClient::ClientLoop() {
  auto& current = co_await Omni::Fiber::GetCurrentFiber();

  while (!_Stop.IsTriggered()) {
    if (_State == kNegotiating) {
      _CurrentCookie = _Prng();
      BOOST_LOG_TRIVIAL(info) << "UdpDynMuxClient(" << this << ") starting negotiation with cookie: " << _CurrentCookie;

      bool negotiated = false;
      while (_State == kNegotiating && !negotiated && !_Stop.IsTriggered()) {
        std::array<uint8_t, 7> buf;
        UdpDynMux::ClientReqId{_CurrentCookie}.Serialize(buf);
        co_await _Socket.async_send(boost::asio::buffer(buf), Omni::Fiber::AsioUseFiber);

        Cancel timerCancel;
        auto timeoutFiber = current.Spawn("negotiate_timeout", [&]() -> Omni::Fiber::Coroutine<void> {
          boost::asio::steady_timer timer(_Socket.get_executor());
          timer.expires_after(std::chrono::seconds(1));
          co_await timer.async_wait(
              boost::asio::bind_cancellation_slot(timerCancel.AsioSlot().Slot(), Omni::Fiber::AsioUseFiber));
          if (!timerCancel.IsTriggered()) {
            co_await _ControlPipe.GetProducer().Put(ControlEvent{.Type = static_cast<UdpDynMux::MsgType>(0)});
          }
          co_return;
        });

        std::optional<ControlEvent> evt;
        co_await Omni::Fiber::Select(
            Omni::Fiber::SelectPair(_Stop.GetFiberCancelEvent(), [&]() {}),
            Omni::Fiber::SelectPair(_ControlPipe.GetConsumer(), [&](auto data) {
              if (data.has_value()) {
                auto& e = data.value();
                if (e.Type == static_cast<UdpDynMux::MsgType>(0) ||
                    (e.Type == UdpDynMux::MsgType::kServerAssignId && e.Cookie == _CurrentCookie)) {
                  evt = e;
                }
              }
            }));

        timerCancel.Trigger();
        co_await current.Join(timeoutFiber);

        if (evt.has_value()) {
          if (evt->Type != static_cast<UdpDynMux::MsgType>(0)) {
            _AssignedId = evt->Id;
            if (_AssignedId == 0) {
              BOOST_LOG_TRIVIAL(warning) << "UdpDynMuxClient(" << this
                                         << ") negotiation failed: no free IDs, retrying in 5 seconds";
              boost::asio::steady_timer waitTimer(_Socket.get_executor());
              waitTimer.expires_after(std::chrono::seconds(5));
              co_await waitTimer.async_wait(Omni::Fiber::AsioUseFiber);
            } else {
              _State = kRunning;
              _LastKeepaliveReceived = std::chrono::steady_clock::now();
              BOOST_LOG_TRIVIAL(info) << "UdpDynMuxClient(" << this << ") successfully negotiated ID: " << _AssignedId;
              negotiated = true;
            }
          } else {
            BOOST_LOG_TRIVIAL(info) << "UdpDynMuxClient(" << this << ") negotiation timeout, retrying...";
          }
        }
      }
    }

    if (_State == kRunning) {
      Cancel keepaliveCancel;
      auto keepaliveFiber = current.Spawn("keepalive_timer", [&]() -> Omni::Fiber::Coroutine<void> {
        auto slotTracker = keepaliveCancel.AsioSlot();
        while (!keepaliveCancel.IsTriggered() && !_Stop.IsTriggered()) {
          boost::asio::steady_timer timer(_Socket.get_executor());
          timer.expires_after(std::chrono::seconds(5));
          auto [err] = co_await timer.async_wait(
              boost::asio::bind_cancellation_slot(slotTracker.Slot(), Omni::Fiber::AsioUseFiber));
          if (err || keepaliveCancel.IsTriggered()) {
            break;
          }
          co_await _ControlPipe.GetProducer().Put(ControlEvent{.Type = static_cast<UdpDynMux::MsgType>(0xFF)});
        }
        co_return;
      });

      bool stateChanged = false;
      while (_State == kRunning && !stateChanged && !_Stop.IsTriggered()) {
        co_await Omni::Fiber::Select(
            Omni::Fiber::SelectPair(_Stop.GetFiberCancelEvent(), [&]() {}),
            Omni::Fiber::SelectPair(_ControlPipe.GetConsumer(), [&](auto data) {
              if (data.has_value()) {
                auto& e = data.value();
                if (e.Type == static_cast<UdpDynMux::MsgType>(0xFF)) {
                  auto now = std::chrono::steady_clock::now();
                  if (now - _LastKeepaliveReceived > std::chrono::seconds(15)) {
                    BOOST_LOG_TRIVIAL(warning)
                        << "UdpDynMuxClient(" << this << ") keepalive timeout, re-negotiating...";
                    _State = kNegotiating;
                    stateChanged = true;
                  }
                } else if (e.Type == UdpDynMux::MsgType::kServerKeepalive && e.Id == _AssignedId) {
                  _LastKeepaliveReceived = std::chrono::steady_clock::now();
                  auto buf = std::make_shared<std::array<uint8_t, 5>>();
                  UdpDynMux::ClientKeepaliveAck{_AssignedId}.Serialize(*buf);
                  _Socket.async_send(boost::asio::buffer(*buf), [buf](const ErrorCode&, std::size_t) {});
                } else if (e.Type == UdpDynMux::MsgType::kServerIdClosed && e.Id == _AssignedId) {
                  BOOST_LOG_TRIVIAL(warning) << "UdpDynMuxClient(" << this << ") server closed ID, re-negotiating...";
                  _State = kNegotiating;
                  stateChanged = true;
                } else if (e.Type == UdpDynMux::MsgType::kServerAddrMismatch && e.Id == _AssignedId) {
                  BOOST_LOG_TRIVIAL(warning)
                      << "UdpDynMuxClient(" << this << ") server reported address mismatch, migrating...";
                  _State = kMigrating;
                  stateChanged = true;
                }
              }
            }));
      }

      keepaliveCancel.Trigger();
      co_await current.Join(keepaliveFiber);
    }

    if (_State == kMigrating) {
      BOOST_LOG_TRIVIAL(info) << "UdpDynMuxClient(" << this << ") starting address migration for ID: " << _AssignedId;
      bool migrated = false;
      while (_State == kMigrating && !migrated && !_Stop.IsTriggered()) {
        auto buf = std::make_shared<std::array<uint8_t, 9>>();
        UdpDynMux::ClientAddrMigrate{_AssignedId, _CurrentCookie}.Serialize(*buf);
        co_await _Socket.async_send(boost::asio::buffer(*buf), Omni::Fiber::AsioUseFiber);

        Cancel timerCancel;
        auto timeoutFiber = current.Spawn("migrate_timeout", [&]() -> Omni::Fiber::Coroutine<void> {
          boost::asio::steady_timer timer(_Socket.get_executor());
          timer.expires_after(std::chrono::seconds(1));
          co_await timer.async_wait(
              boost::asio::bind_cancellation_slot(timerCancel.AsioSlot().Slot(), Omni::Fiber::AsioUseFiber));
          if (!timerCancel.IsTriggered()) {
            co_await _ControlPipe.GetProducer().Put(ControlEvent{.Type = static_cast<UdpDynMux::MsgType>(0)});
          }
          co_return;
        });

        std::optional<ControlEvent> evt;
        co_await Omni::Fiber::Select(
            Omni::Fiber::SelectPair(_Stop.GetFiberCancelEvent(), [&]() {}),
            Omni::Fiber::SelectPair(_ControlPipe.GetConsumer(), [&](auto data) {
              if (data.has_value()) {
                auto& e = data.value();
                if (e.Type == static_cast<UdpDynMux::MsgType>(0) ||
                    (e.Type == UdpDynMux::MsgType::kServerMigrateAck && e.Id == _AssignedId) ||
                    (e.Type == UdpDynMux::MsgType::kServerCookieMismatch && e.Id == _AssignedId)) {
                  evt = e;
                }
              }
            }));

        timerCancel.Trigger();
        co_await current.Join(timeoutFiber);

        if (evt.has_value()) {
          if (evt->Type == UdpDynMux::MsgType::kServerCookieMismatch) {
            BOOST_LOG_TRIVIAL(warning) << "UdpDynMuxClient(" << this
                                       << ") server reported cookie mismatch during migration, re-negotiating...";
            _State = kNegotiating;
          } else if (evt->Type == UdpDynMux::MsgType::kServerMigrateAck) {
            _State = kRunning;
            _LastKeepaliveReceived = std::chrono::steady_clock::now();
            BOOST_LOG_TRIVIAL(info) << "UdpDynMuxClient(" << this << ") address migration successful";
            migrated = true;
          }
        } else {
          BOOST_LOG_TRIVIAL(info) << "UdpDynMuxClient(" << this << ") retrying migration request...";
        }
      }
    }
  }
}

Omni::Fiber::Coroutine<void> UdpDynMuxClient::HandleControlPacket(const uint8_t* data, std::size_t length) {
  if (length < 3) {
    co_return;
  }

  uint8_t msgType = data[2];
  if (msgType == static_cast<uint8_t>(UdpDynMux::MsgType::kServerAssignId)) {
    if (auto msg = UdpDynMux::ServerAssignId::Deserialize(data, length)) {
      co_await _ControlPipe.GetProducer().Put(
          ControlEvent{.Type = UdpDynMux::MsgType::kServerAssignId, .Cookie = msg->Cookie, .Id = msg->AssignedId});
    }
  } else if (msgType == static_cast<uint8_t>(UdpDynMux::MsgType::kServerKeepalive)) {
    if (auto msg = UdpDynMux::ServerKeepalive::Deserialize(data, length)) {
      co_await _ControlPipe.GetProducer().Put(
          ControlEvent{.Type = UdpDynMux::MsgType::kServerKeepalive, .Id = msg->Id});
    }
  } else if (msgType == static_cast<uint8_t>(UdpDynMux::MsgType::kServerIdClosed)) {
    if (auto msg = UdpDynMux::ServerIdClosed::Deserialize(data, length)) {
      co_await _ControlPipe.GetProducer().Put(ControlEvent{.Type = UdpDynMux::MsgType::kServerIdClosed, .Id = msg->Id});
    }
  } else if (msgType == static_cast<uint8_t>(UdpDynMux::MsgType::kServerAddrMismatch)) {
    if (auto msg = UdpDynMux::ServerAddrMismatch::Deserialize(data, length)) {
      co_await _ControlPipe.GetProducer().Put(
          ControlEvent{.Type = UdpDynMux::MsgType::kServerAddrMismatch, .Id = msg->Id});
    }
  } else if (msgType == static_cast<uint8_t>(UdpDynMux::MsgType::kServerMigrateAck)) {
    if (auto msg = UdpDynMux::ServerMigrateAck::Deserialize(data, length)) {
      co_await _ControlPipe.GetProducer().Put(
          ControlEvent{.Type = UdpDynMux::MsgType::kServerMigrateAck, .Id = msg->Id});
    }
  } else if (msgType == static_cast<uint8_t>(UdpDynMux::MsgType::kServerCookieMismatch)) {
    if (auto msg = UdpDynMux::ServerCookieMismatch::Deserialize(data, length)) {
      co_await _ControlPipe.GetProducer().Put(
          ControlEvent{.Type = UdpDynMux::MsgType::kServerCookieMismatch, .Id = msg->Id});
    }
  }
}

} // namespace gh
