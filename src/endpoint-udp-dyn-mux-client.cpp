#include "endpoint-udp-dyn-mux-client.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/log/trivial.hpp>
#include <cassert>
#include <random>

#include "endpoint-udp-dyn-mux-protocol.hpp"

namespace gh {

UdpDynMuxClient::UdpDynMuxClient(boost::asio::io_context& io_context, boost::asio::ip::udp::endpoint peer)
    : _Socket(io_context), _Peer(peer), _Local(boost::asio::ip::udp::v6(), 0), _NegotiateTimer(io_context),
      _MigrateTimer(io_context), _KeepaliveCheckTimer(io_context) {
  std::random_device rd;
  _Prng.seed(rd());
}

UdpDynMuxClient::UdpDynMuxClient(boost::asio::io_context& io_context, boost::asio::ip::udp::endpoint peer,
                                 boost::asio::ip::udp::endpoint local)
    : _Socket(io_context), _Peer(peer), _Local(local), _NegotiateTimer(io_context), _MigrateTimer(io_context),
      _KeepaliveCheckTimer(io_context) {
  std::random_device rd;
  _Prng.seed(rd());
}

void UdpDynMuxClient::AsyncStart(std::move_only_function<Event>&& handler) {
  switch (_State) {
  case kNone:
    try {
      _Socket.open(boost::asio::ip::udp::v6());
      _Socket.set_option(boost::asio::ip::v6_only(false));
      _Socket.bind(_Local);
      BOOST_LOG_TRIVIAL(info) << "UdpDynMuxClient(" << this << ") bound at " << _Socket.local_endpoint();
    } catch (const SystemError& e) {
      BOOST_LOG_TRIVIAL(error) << "UdpDynMuxClient(" << this << ") start failed: " << e.what();
      _State = kError;
      handler(e.code());
      return;
    }

    _Socket.async_connect(_Peer, [me = shared_from_this(), handler{std::move(handler)}](const ErrorCode& ec) mutable {
      if (ec) {
        BOOST_LOG_TRIVIAL(error) << "UdpDynMuxClient(" << &*me << ") connect failed: " << ec.message();
        me->_State = kError;
        handler(ec);
        return;
      }
      me->StartNegotiation();
      me->ScheduleSocketRead();
      handler(ErrorCode());
    });
    break;
  case kNegotiating:
  case kRunning:
  case kMigrating:
    handler(ErrorCode());
    break;
  default:
    handler(ErrorCode{AppErrorCategory::kIncorrectState, kAppError});
    break;
  }
}

void UdpDynMuxClient::StartNegotiation() {
  _State = kNegotiating;
  _CurrentCookie = _Prng();
  BOOST_LOG_TRIVIAL(info) << "UdpDynMuxClient(" << this << ") starting negotiation with cookie: " << _CurrentCookie;
  SendNegotiateRequest();
}

void UdpDynMuxClient::SendNegotiateRequest() {
  if (_State != kNegotiating) {
    return;
  }

  auto buf = std::make_shared<std::array<uint8_t, 7>>();
  UdpDynMux::ClientReqId{_CurrentCookie}.Serialize(*buf);

  _Socket.async_send(boost::asio::buffer(*buf), [me = shared_from_this(), buf](const ErrorCode& ec, std::size_t) {
    // Handled by timer retransmissions
  });

  _NegotiateTimer.expires_after(std::chrono::seconds(1));
  _NegotiateTimer.async_wait([me = shared_from_this()](const ErrorCode& ec) {
    if (!ec && me->_State == kNegotiating) {
      BOOST_LOG_TRIVIAL(info) << "UdpDynMuxClient(" << &*me << ") retrying negotiation request...";
      me->SendNegotiateRequest();
    }
  });
}

void UdpDynMuxClient::StartMigration() {
  _State = kMigrating;
  BOOST_LOG_TRIVIAL(info) << "UdpDynMuxClient(" << this << ") starting address migration for ID: " << _AssignedId;
  SendMigrationRequest();
}

void UdpDynMuxClient::SendMigrationRequest() {
  if (_State != kMigrating) {
    return;
  }

  auto buf = std::make_shared<std::array<uint8_t, 9>>();
  UdpDynMux::ClientAddrMigrate{_AssignedId, _CurrentCookie}.Serialize(*buf);

  _Socket.async_send(boost::asio::buffer(*buf), [me = shared_from_this(), buf](const ErrorCode& ec, std::size_t) {
    // Handled by timer retransmissions
  });

  _MigrateTimer.expires_after(std::chrono::seconds(1));
  _MigrateTimer.async_wait([me = shared_from_this()](const ErrorCode& ec) {
    if (!ec && me->_State == kMigrating) {
      BOOST_LOG_TRIVIAL(info) << "UdpDynMuxClient(" << &*me << ") retrying migration request...";
      me->SendMigrationRequest();
    }
  });
}

void UdpDynMuxClient::CheckKeepaliveTimeout() {
  _KeepaliveCheckTimer.expires_after(std::chrono::seconds(5));
  _KeepaliveCheckTimer.async_wait([me = shared_from_this()](const ErrorCode& ec) {
    if (ec || me->_State != kRunning) {
      return;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - me->_LastKeepaliveReceived > std::chrono::seconds(15)) {
      BOOST_LOG_TRIVIAL(warning) << "UdpDynMuxClient(" << &*me << ") keepalive timeout from server, re-negotiating...";
      me->StartNegotiation();
    } else {
      me->CheckKeepaliveTimeout();
    }
  });
}

void UdpDynMuxClient::ScheduleSocketRead() {
  if (_State == kError || _State == kNone) {
    return;
  }

  auto a = std::make_shared<std::array<uint8_t, 2048>>();
  auto p = Packet{Buffer(*a), a};
  auto buffer = boost::asio::mutable_buffer{p.first};

  _Socket.async_receive(buffer, [me = shared_from_this(), p{std::move(p)}](const ErrorCode& ec,
                                                                           std::size_t bytes_transferred) mutable {
    if (ec) {
      if (ec != boost::asio::error::operation_aborted) {
        BOOST_LOG_TRIVIAL(error) << "UdpDynMuxClient(" << &*me << ") read error: " << ec.message();
        me->_State = kError;
        if (me->_ReadHandlerCb) {
          auto h = std::move(me->_ReadHandlerCb);
          me->_ReadHandlerCb = nullptr;
          auto a_dummy = std::make_shared<std::array<uint8_t, 0>>();
          h(ec, Packet{Buffer(*a_dummy), a_dummy});
        }
      }
      return;
    }

    auto& buf = p.first;
    assert(bytes_transferred <= buf.Capacity - buf.Offset);
    buf.Length = bytes_transferred;

    if (bytes_transferred >= 2) {
      uint16_t channel_id = (buf.Data[buf.Offset] << 8) | buf.Data[buf.Offset + 1];
      if (channel_id == 0) {
        me->HandleControlPacket(buf.Data + buf.Offset, buf.Length);
      } else if (me->_State == kRunning && channel_id == me->_AssignedId) {
        buf.Offset += 2;
        buf.Length -= 2;

        if (me->_ReadHandlerCb) {
          auto h = std::move(me->_ReadHandlerCb);
          me->_ReadHandlerCb = nullptr;
          h(ErrorCode(), std::move(p));
        } else {
          if (me->_IncomingDataQueue.size() < 1000) {
            me->_IncomingDataQueue.push(std::move(p));
          } else {
            BOOST_LOG_TRIVIAL(warning) << "UdpDynMuxClient(" << &*me << ") incoming data queue full, dropping packet";
          }
        }
      } else {
        BOOST_LOG_TRIVIAL(debug) << "UdpDynMuxClient(" << &*me
                                 << ") ignored data packet for unknown channel ID: " << channel_id;
      }
    }
    me->ScheduleSocketRead();
  });
}

void UdpDynMuxClient::HandleControlPacket(const uint8_t* data, std::size_t length) {
  if (length < 3) {
    return;
  }

  uint8_t msg_type = data[2];
  if (msg_type == UdpDynMux::MsgType::kServerAssignId) {
    if (auto msg = UdpDynMux::ServerAssignId::Deserialize(data, length)) {
      if (_State == kNegotiating && msg->Cookie == _CurrentCookie) {
        _NegotiateTimer.cancel();
        _AssignedId = msg->AssignedId;
        if (_AssignedId == 0) {
          BOOST_LOG_TRIVIAL(warning) << "UdpDynMuxClient(" << this << ") negotiation failed: no free IDs";
          _NegotiateTimer.expires_after(std::chrono::seconds(5));
          _NegotiateTimer.async_wait([me = shared_from_this()](const ErrorCode& ec) {
            if (!ec) {
              me->StartNegotiation();
            }
          });
        } else {
          _State = kRunning;
          _LastKeepaliveReceived = std::chrono::steady_clock::now();
          BOOST_LOG_TRIVIAL(info) << "UdpDynMuxClient(" << this << ") successfully negotiated ID: " << _AssignedId;
          CheckKeepaliveTimeout();
          FlushWriteQueue();
        }
      }
    }
  } else if (msg_type == UdpDynMux::MsgType::kServerKeepalive) {
    if (auto msg = UdpDynMux::ServerKeepalive::Deserialize(data, length)) {
      if (_State == kRunning && msg->Id == _AssignedId) {
        _LastKeepaliveReceived = std::chrono::steady_clock::now();
        auto buf = std::make_shared<std::array<uint8_t, 5>>();
        UdpDynMux::ClientKeepaliveAck{_AssignedId}.Serialize(*buf);
        _Socket.async_send(boost::asio::buffer(*buf), [buf](const ErrorCode&, std::size_t) {});
      }
    }
  } else if (msg_type == UdpDynMux::MsgType::kServerIdClosed) {
    if (auto msg = UdpDynMux::ServerIdClosed::Deserialize(data, length)) {
      if (_State == kRunning && msg->Id == _AssignedId) {
        BOOST_LOG_TRIVIAL(warning) << "UdpDynMuxClient(" << this << ") server closed ID, re-negotiating...";
        StartNegotiation();
      }
    }
  } else if (msg_type == UdpDynMux::MsgType::kServerAddrMismatch) {
    if (auto msg = UdpDynMux::ServerAddrMismatch::Deserialize(data, length)) {
      if (_State == kRunning && msg->Id == _AssignedId) {
        BOOST_LOG_TRIVIAL(warning) << "UdpDynMuxClient(" << this << ") server reported address mismatch, migrating...";
        StartMigration();
      }
    }
  } else if (msg_type == UdpDynMux::MsgType::kServerMigrateAck) {
    if (auto msg = UdpDynMux::ServerMigrateAck::Deserialize(data, length)) {
      if (_State == kMigrating && msg->Id == _AssignedId) {
        _MigrateTimer.cancel();
        _State = kRunning;
        BOOST_LOG_TRIVIAL(info) << "UdpDynMuxClient(" << this << ") address migration successful";
      }
    }
  }
}

void UdpDynMuxClient::AsyncRead(std::move_only_function<ReadHandler>&& handler) {
  if (_State == kError) {
    auto a_dummy = std::make_shared<std::array<uint8_t, 1>>();
    handler(ErrorCode(AppErrorCategory::kIncorrectState, kAppError), Packet{Buffer(*a_dummy), a_dummy});
    return;
  }

  if (!_IncomingDataQueue.empty()) {
    auto p = std::move(_IncomingDataQueue.front());
    _IncomingDataQueue.pop();
    handler(ErrorCode(), std::move(p));
  } else {
    assert(!_ReadHandlerCb);
    _ReadHandlerCb = std::move(handler);
  }
}

void UdpDynMuxClient::AsyncWrite(Packet&& p, std::move_only_function<WriteHandler>&& handler) {
  if (_State == kError) {
    handler(ErrorCode(AppErrorCategory::kIncorrectState, kAppError), 0);
    return;
  }

  if (_State == kNone || _State == kNegotiating || _State == kMigrating) {
    _WriteQueue.push({std::move(p), std::move(handler)});
    return;
  }

  assert(!_WritePending);
  _WritePending = true;

  auto& buf = p.first;
  if (buf.Offset < 2) {
    handler(ErrorCode{AppErrorCategory::kInvalidPacketReserved, kAppError}, 0);
    _WritePending = false;
    return;
  }

  buf.Offset -= 2;
  buf.Length += 2;
  buf.Data[buf.Offset] = (_AssignedId >> 8) & 0xFF;
  buf.Data[buf.Offset + 1] = _AssignedId & 0xFF;

  _Socket.async_send(boost::asio::const_buffer{buf},
                     [me = shared_from_this(), handler = std::move(handler)](const ErrorCode& ec,
                                                                             std::size_t bytes_transferred) mutable {
                       me->_WritePending = false;
                       handler(ec, bytes_transferred);
                       me->FlushWriteQueue();
                     });
}

void UdpDynMuxClient::FlushWriteQueue() {
  if (_State != kRunning || _WriteQueue.empty() || _WritePending) {
    return;
  }
  auto p = std::move(_WriteQueue.front());
  _WriteQueue.pop();
  AsyncWrite(std::move(p.first), std::move(p.second));
}

} // namespace gh
