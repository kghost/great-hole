#include "endpoint-udp-mux-server.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/log/trivial.hpp>
#include <cassert>

#include "scoped_flag.hpp"

namespace gh {

class UdpMuxServer::Channel : public Endpoint {
public:
  Channel(std::shared_ptr<UdpMuxServer> parent, uint8_t id) : _Parent(parent), _Id(id) {}

  void AsyncStart(std::move_only_function<Event>&& handler) override { _Parent->TryAsyncStart(std::move(handler)); }

  void AsyncRead(std::move_only_function<ReadHandler>&& handler) override { _Parent->Read(_Id, std::move(handler)); }

  void AsyncWrite(Packet&& p, std::move_only_function<WriteHandler>&& handler) override {
    _Parent->Write(_Id, std::move(p), std::move(handler));
  }

private:
  std::shared_ptr<UdpMuxServer> _Parent;
  uint8_t _Id;
};

std::shared_ptr<Endpoint> UdpMuxServer::CreateChannel(uint8_t id) {
  return std::shared_ptr<Endpoint>(new Channel(shared_from_this(), id));
}

UdpMuxServer::UdpMuxServer(boost::asio::io_context& io_context)
    : _Socket(io_context), _Local(boost::asio::ip::udp::v6(), 0) {}
UdpMuxServer::UdpMuxServer(boost::asio::io_context& io_context, boost::asio::ip::udp::endpoint bind)
    : _Socket(io_context), _Local(bind) {}

void UdpMuxServer::TryAsyncStart(std::move_only_function<Event>&& handler) {
  switch (_State) {
  case kNone:
    AsyncStart(std::move(handler));
    break;
  case kRunning:
    handler(ErrorCode());
    break;
  default:
    handler(ErrorCode{AppErrorCategory::kIncorrectState, kAppError});
    break;
  }
}

void UdpMuxServer::AsyncStart(std::move_only_function<Event>&& handler) {
  try {
    _Socket.open(boost::asio::ip::udp::v6());
    _Socket.set_option(boost::asio::ip::v6_only(false));
    _Socket.bind(_Local);
    BOOST_LOG_TRIVIAL(info) << "UdpMuxServer(" << this << ") bound at " << _Socket.local_endpoint();
    _State = kRunning;
    handler(ErrorCode());
  } catch (const SystemError& e) {
    BOOST_LOG_TRIVIAL(info) << "UdpMuxServer(" << this << ") start failed: " << e.what();
    _State = kError;
    handler(e.code());
  }
}

void UdpMuxServer::Read(uint8_t id, std::move_only_function<ReadHandler>&& handler) {
  if (_State != kRunning) {
    return;
  }
  std::shared_ptr<ReadHandlerMap> m;
  if ((m = _ReadingChannel.lock())) {
    assert(m->find(id) == m->end());
  } else {
    m.reset(new ReadHandlerMap);
    _ReadingChannel = m;
  }
  m->emplace(id, std::move(handler));
  ScheduleRead(m);
}

void UdpMuxServer::ScheduleRead(std::shared_ptr<ReadHandlerMap> m) {
  if (_State != kRunning || _ReadPending) {
    return;
  }
  _ReadPending = true;

  auto a = std::make_shared<std::array<uint8_t, 2048>>();
  auto p = Packet{Buffer(*a), a};
  auto buffer = boost::asio::mutable_buffer{p.first};
  auto peer_address = std::make_shared<boost::asio::ip::udp::endpoint>();
  _Socket.async_receive_from(buffer, *peer_address,
                             [me = shared_from_this(), p{std::move(p)}, m,
                              peer_address](const ErrorCode& ec, std::size_t bytes_transferred) mutable {
                               me->_ReadPending = false;
                               if (!ec) {
                                 auto& buf = p.first;

                                 assert(bytes_transferred <= buf.Capacity - buf.Offset);
                                 buf.Length = bytes_transferred;

                                 if (bytes_transferred >= 1) {
                                   uint8_t id = buf.Data[buf.Offset];
                                   buf.Offset += 1;
                                   buf.Length -= 1;
                                   me->_Peers[id] = *peer_address; // learn peer address

                                   auto h = m->find(id);
                                   if (h != m->end()) {
                                     auto handler = std::move(h->second);
                                     m->erase(h);
                                     handler(ec, std::move(p));
                                   } else {
                                     BOOST_LOG_TRIVIAL(info)
                                         << "UdpMuxServer(" << &*me << ") packet from unknown peer: " << (int)id;
                                   }
                                 }
                               } else if (ec != boost::asio::error::operation_aborted) {
                                 BOOST_LOG_TRIVIAL(error)
                                     << "UdpMuxServer(" << &*me << ") read error: " << ec.message();
                               }
                               if (!m->empty()) {
                                 me->ScheduleRead(m);
                               }
                             });
}

void UdpMuxServer::Write(uint8_t id, Packet&& p, std::move_only_function<WriteHandler>&& handler) {
  if (_State != kRunning) {
    return;
  }
  _WriteQueue.push(std::make_tuple(id, std::move(p), std::move(handler)));
  if (_State != kRunning || _WritePending) {
    return;
  }
  ScheduleWrite(ScopedFlag(_WritePending));
}

void UdpMuxServer::ScheduleWrite(ScopedFlag&& write) {
  auto& next = _WriteQueue.front();
  auto id = std::get<0>(next);
  auto p = std::move(std::get<1>(next));
  auto handler = std::move(std::get<2>(next));
  _WriteQueue.pop();

  auto peer_iter = _Peers.find(id);
  if (peer_iter == _Peers.end()) {
    handler(ErrorCode{AppErrorCategory::kInvalidPacketSession, kAppError}, 0);
    if (!_WriteQueue.empty()) {
      ScheduleWrite(std::move(write));
    }
    return;
  }

  auto& buf = p.first;
  if (buf.Offset < 1) {
    handler(ErrorCode{AppErrorCategory::kInvalidPacketReserved, kAppError}, 0);
    if (!_WriteQueue.empty()) {
      ScheduleWrite(std::move(write));
    }
    return;
  }

  buf.Offset -= 1;
  buf.Length += 1;
  buf.Data[buf.Offset] = id;

  _Socket.async_send_to(boost::asio::const_buffer{buf}, peer_iter->second,
                        [me = shared_from_this(), handler = std::move(handler),
                         write = std::move(write)](const ErrorCode& ec, std::size_t bytes_transferred) mutable {
                          handler(ec, bytes_transferred);
                          if (!me->_WriteQueue.empty()) {
                            me->ScheduleWrite(std::move(write));
                          }
                        });
}

} // namespace gh
