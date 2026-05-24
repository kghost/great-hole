#include "endpoint-udp-dyn-mux-server.hpp"

#include <algorithm>
#include <boost/asio/buffer.hpp>
#include <boost/log/trivial.hpp>
#include <cassert>

#include "endpoint-udp-dyn-mux-protocol.hpp"

namespace gh {

UdpDynMuxServer::UdpDynMuxServer(boost::asio::io_context& io_context)
    : _Socket(io_context), _Local(boost::asio::ip::udp::v6(), 0), _KeepaliveTimer(io_context) {}

UdpDynMuxServer::UdpDynMuxServer(boost::asio::io_context& io_context, boost::asio::ip::udp::endpoint bind)
    : _Socket(io_context), _Local(bind), _KeepaliveTimer(io_context) {}

void UdpDynMuxServer::AsyncStart(std::move_only_function<Event>&& handler) {
  switch (_State) {
  case kNone:
    try {
      _Socket.open(boost::asio::ip::udp::v6());
      _Socket.set_option(boost::asio::ip::v6_only(false));
      _Socket.bind(_Local);
      BOOST_LOG_TRIVIAL(info) << "UdpDynMuxServer(" << this << ") bound at " << _Socket.local_endpoint();
      _State = kRunning;
      StartKeepaliveTimer();
      ScheduleRead();
      handler(ErrorCode());
    } catch (const SystemError& e) {
      BOOST_LOG_TRIVIAL(error) << "UdpDynMuxServer(" << this << ") start failed: " << e.what();
      _State = kError;
      handler(e.code());
    }
    break;
  case kRunning:
    handler(ErrorCode());
    break;
  default:
    handler(ErrorCode{AppErrorCategory::kIncorrectState, kAppError});
    break;
  }
}

void UdpDynMuxServer::ScheduleRead() {
  if (_State != kRunning || _ReadPending) {
    return;
  }
  _ReadPending = true;

  auto a = std::make_shared<std::array<uint8_t, 2048>>();
  auto p = Packet{Buffer(*a), a};
  auto buffer = boost::asio::mutable_buffer{p.first};
  auto peer_address = std::make_shared<boost::asio::ip::udp::endpoint>();

  _Socket.async_receive_from(
      buffer, *peer_address,
      [me = shared_from_this(), p{std::move(p)}, peer_address](const ErrorCode& ec,
                                                               std::size_t bytes_transferred) mutable {
        me->_ReadPending = false;
        if (!ec) {
          auto& buf = p.first;
          assert(bytes_transferred <= buf.Capacity - buf.Offset);
          buf.Length = bytes_transferred;

          if (bytes_transferred >= 2) {
            uint16_t channel_id = (buf.Data[buf.Offset] << 8) | buf.Data[buf.Offset + 1];
            auto now = std::chrono::steady_clock::now();

            if (channel_id == 0) {
              // Control packet
              if (bytes_transferred >= 3) {
                uint8_t msg_type = buf.Data[buf.Offset + 2];
                if (msg_type == UdpDynMux::MsgType::kClientReqId) {
                  if (auto req = UdpDynMux::ClientReqId::Deserialize(buf.Data + buf.Offset, bytes_transferred)) {
                    uint32_t cookie = req->Cookie;

                    // Check if this endpoint already has a session
                    auto it = std::ranges::find_if(
                        me->_Clients, [peer_address](const auto& pair) { return pair.second.Peer == *peer_address; });
                    if (it != me->_Clients.end()) {
                      auto& c = it->second;
                      c.Cookie = cookie;
                      c.LastSeen = now;
                      c.MissingAcks = 0;
                      BOOST_LOG_TRIVIAL(info) << "UdpDynMuxServer(" << &*me << ") re-assigning ID: " << it->first
                                              << " to peer: " << *peer_address;
                      me->SendControlAssign(*peer_address, cookie, it->first);
                    } else {
                      // Find a free channel ID dynamically
                      uint16_t free_id = 0;
                      for (uint16_t candidate = 1; candidate < 65535; ++candidate) {
                        if (me->_Clients.find(candidate) == me->_Clients.end()) {
                          free_id = candidate;
                          break;
                        }
                      }

                      if (free_id != 0) {
                        auto [it, ok] = me->_Clients.emplace(
                            free_id, ClientSession{.Peer = *peer_address, .Cookie = cookie, .LastSeen = now});
                        assert(ok);
                        BOOST_LOG_TRIVIAL(info) << "UdpDynMuxServer(" << &*me << ") assigned dynamic ID: " << free_id
                                                << " to peer: " << *peer_address;
                        me->SendControlAssign(*peer_address, cookie, free_id);
                      } else {
                        BOOST_LOG_TRIVIAL(warning)
                            << "UdpDynMuxServer(" << &*me << ") allocation failed: no free channels";
                        // TODO: need an error mesaage rather than id 0 channel
                        me->SendControlAssign(*peer_address, cookie, 0);
                      }
                    }
                  }
                } else if (msg_type == UdpDynMux::MsgType::kClientKeepaliveAck) {
                  if (auto ack = UdpDynMux::ClientKeepaliveAck::Deserialize(buf.Data + buf.Offset, bytes_transferred)) {
                    uint16_t id = ack->Id;
                    auto peer_it = me->_Clients.find(id);
                    if (peer_it != me->_Clients.end()) {
                      auto& c = peer_it->second;
                      if (c.Peer == *peer_address) {
                        c.MissingAcks = 0;
                        c.LastSeen = now;
                      } else {
                        me->SendControlAddrMismatch(*peer_address, id);
                      }
                    } else {
                      me->SendControlInvalidId(*peer_address, id);
                    }
                  }
                } else if (msg_type == UdpDynMux::MsgType::kClientAddrMigrate) {
                  if (auto migrate =
                          UdpDynMux::ClientAddrMigrate::Deserialize(buf.Data + buf.Offset, bytes_transferred)) {
                    uint16_t id = migrate->Id;
                    uint32_t cookie = migrate->Cookie;

                    auto peer_it = me->_Clients.find(id);
                    if (peer_it != me->_Clients.end()) {
                      auto& c = peer_it->second;
                      if (c.Cookie == cookie) {
                        BOOST_LOG_TRIVIAL(info) << "UdpDynMuxServer(" << &*me << ") migrating ID " << id << " from "
                                                << c.Peer << " to " << *peer_address;
                        c.Peer = *peer_address;
                        c.LastSeen = now;
                        c.MissingAcks = 0;
                        me->SendControlMigrateAck(*peer_address, id);
                      } else {
                        BOOST_LOG_TRIVIAL(warning)
                            << "UdpDynMuxServer(" << &*me << ") migration cookie mismatch for ID " << id << " from "
                            << c.Peer << " to " << *peer_address << "; cookie: " << c.Cookie << " != " << cookie;
                        me->SendControlCookieMismatch(*peer_address, id);
                      }
                    } else {
                      me->SendControlInvalidId(*peer_address, id);
                    }
                  }
                }
              }
            } else {
              // Data packet
              auto peer_it = me->_Clients.find(channel_id);
              if (peer_it != me->_Clients.end()) {
                auto& c = peer_it->second;
                if (c.Peer == *peer_address) {
                  c.MissingAcks = 0;
                  c.LastSeen = now;
                  BOOST_LOG_TRIVIAL(debug) << "UdpDynMuxServer(" << &*me << ") data packet for channel ID "
                                           << channel_id << " ignored (no pipeline/handler bound)";
                } else {
                  me->SendControlAddrMismatch(*peer_address, channel_id);
                }
              } else {
                me->SendControlInvalidId(*peer_address, channel_id);
              }
            }
          }
        } else {
          if (ec != boost::asio::error::operation_aborted) {
            BOOST_LOG_TRIVIAL(error) << "UdpDynMuxServer(" << &*me << ") read error: " << ec.message();
          }
        }
        me->ScheduleRead();
      });
}

void UdpDynMuxServer::StartKeepaliveTimer() {
  _KeepaliveTimer.expires_after(std::chrono::seconds(5));
  _KeepaliveTimer.async_wait([me = shared_from_this()](const ErrorCode& ec) {
    if (ec || me->_State != kRunning) {
      return;
    }
    me->HandleKeepaliveTick();
    me->StartKeepaliveTimer();
  });
}

void UdpDynMuxServer::HandleKeepaliveTick() {
  std::erase_if(_Clients, [](const auto& it) { return it.second.MissingAcks >= 3; });
  for (auto& [id, c] : _Clients) {
    c.MissingAcks++;
    if (std::chrono::steady_clock::now() > c.LastSeen + std::chrono::seconds(5)) {
      SendControlKeepalive(c.Peer, id);
    }
  }
}

void UdpDynMuxServer::SendControlAssign(const boost::asio::ip::udp::endpoint& peer, uint32_t cookie, uint16_t id) {
  auto buf = std::make_shared<std::array<uint8_t, 9>>();
  UdpDynMux::ServerAssignId{cookie, id}.Serialize(*buf);
  _Socket.async_send_to(boost::asio::buffer(*buf), peer, [buf](const ErrorCode&, std::size_t) {});
}

void UdpDynMuxServer::SendControlKeepalive(const boost::asio::ip::udp::endpoint& peer, uint16_t id) {
  auto buf = std::make_shared<std::array<uint8_t, 5>>();
  UdpDynMux::ServerKeepalive{id}.Serialize(*buf);
  _Socket.async_send_to(boost::asio::buffer(*buf), peer, [buf](const ErrorCode&, std::size_t) {});
}

void UdpDynMuxServer::SendControlIdClosed(const boost::asio::ip::udp::endpoint& peer, uint16_t id) {
  auto buf = std::make_shared<std::array<uint8_t, 5>>();
  UdpDynMux::ServerIdClosed{id}.Serialize(*buf);
  _Socket.async_send_to(boost::asio::buffer(*buf), peer, [buf](const ErrorCode&, std::size_t) {});
}

bool UdpDynMuxServer::CheckRateLimit(const boost::asio::ip::udp::endpoint& peer) {
  auto now = std::chrono::steady_clock::now();
  auto it = _LastErrorSent.find(peer);
  if (it != _LastErrorSent.end() && now - it->second < std::chrono::seconds(1)) {
    return false;
  }
  _LastErrorSent[peer] = now;
  return true;
}

void UdpDynMuxServer::SendControlAddrMismatch(const boost::asio::ip::udp::endpoint& peer, uint16_t id) {
  if (!CheckRateLimit(peer)) {
    return;
  }

  auto buf = std::make_shared<std::array<uint8_t, 5>>();
  UdpDynMux::ServerAddrMismatch{id}.Serialize(*buf);
  _Socket.async_send_to(boost::asio::buffer(*buf), peer, [buf](const ErrorCode&, std::size_t) {});
}

void UdpDynMuxServer::SendControlMigrateAck(const boost::asio::ip::udp::endpoint& peer, uint16_t id) {
  auto buf = std::make_shared<std::array<uint8_t, 5>>();
  UdpDynMux::ServerMigrateAck{id}.Serialize(*buf);
  _Socket.async_send_to(boost::asio::buffer(*buf), peer, [buf](const ErrorCode&, std::size_t) {});
}

void UdpDynMuxServer::SendControlInvalidId(const boost::asio::ip::udp::endpoint& peer, uint16_t id) {
  if (!CheckRateLimit(peer)) {
    return;
  }

  auto buf = std::make_shared<std::array<uint8_t, 5>>();
  UdpDynMux::ServerInvalidId{id}.Serialize(*buf);
  _Socket.async_send_to(boost::asio::buffer(*buf), peer, [buf](const ErrorCode&, std::size_t) {});
}

void UdpDynMuxServer::SendControlCookieMismatch(const boost::asio::ip::udp::endpoint& peer, uint16_t id) {
  if (!CheckRateLimit(peer)) {
    return;
  }

  auto buf = std::make_shared<std::array<uint8_t, 5>>();
  UdpDynMux::ServerCookieMismatch{id}.Serialize(*buf);
  _Socket.async_send_to(boost::asio::buffer(*buf), peer, [buf](const ErrorCode&, std::size_t) {});
}

} // namespace gh
