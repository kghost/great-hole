#include "endpoint-udp.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/log/trivial.hpp>
#include <cassert>

namespace gh {

class Udp::UdpChannel : public Endpoint {
public:
  UdpChannel(std::shared_ptr<Udp> parent, const boost::asio::ip::udp::endpoint& peer) : _Parent(parent), _Peer(peer) {}

  void AsyncStart(std::move_only_function<Event>&& handler) override { _Parent->TryAsyncStart(std::move(handler)); }

  void AsyncRead(std::move_only_function<ReadHandler>&& handler) override { _Parent->Read(_Peer, std::move(handler)); }

  void AsyncWrite(Packet&& p, std::move_only_function<WriteHandler>&& handler) override {
    _Parent->Write(_Peer, std::move(p), std::move(handler));
  }

private:
  std::shared_ptr<Udp> _Parent;
  const boost::asio::ip::udp::endpoint _Peer;
};

std::shared_ptr<Endpoint> Udp::CreateChannel(boost::asio::ip::udp::endpoint const& peer) {
  return std::shared_ptr<Endpoint>(new UdpChannel(shared_from_this(), peer));
}

Udp::Udp(boost::asio::io_context& io_context) : _Socket(io_context), _Local(boost::asio::ip::udp::v6(), 0) {}
Udp::Udp(boost::asio::io_context& io_context, boost::asio::ip::udp::endpoint bind)
    : _Socket(io_context), _Local(bind) {}

void Udp::TryAsyncStart(std::move_only_function<Event>&& handler) {
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

void Udp::AsyncStart(std::move_only_function<Event>&& handler) {
  try {
    _Socket.open(boost::asio::ip::udp::v6());
    _Socket.set_option(boost::asio::ip::v6_only(false));
    _Socket.bind(_Local);
    BOOST_LOG_TRIVIAL(info) << "udp(" << this << ") bound at " << _Socket.local_endpoint();
    _State = kRunning;
    handler(ErrorCode());
  } catch (const SystemError& e) {
    BOOST_LOG_TRIVIAL(info) << "udp(" << this << ") start failed: " << e.what();
    _State = kError;
    handler(e.code());
  }
}

void Udp::Read(boost::asio::ip::udp::endpoint const& peer, std::move_only_function<ReadHandler>&& handler) {
  if (_State != kRunning) {
    return;
  }
  std::shared_ptr<ReadHandlerMap> m;
  if ((m = _ReadingChannel.lock())) {
    assert(m->find(peer) == m->end());
  } else {
    m.reset(new ReadHandlerMap);
    _ReadingChannel = m;
  }
  m->emplace(peer, std::move(handler));
  ScheduleRead(m);
}

void Udp::ScheduleRead(std::shared_ptr<ReadHandlerMap> m) {
  if (_State != kRunning || _ReadPending) {
    return;
  }
  _ReadPending = true;

  auto a = std::make_shared<std::array<uint8_t, 2048>>();
  auto p = Packet{Buffer(*a), a};
  auto buffer = boost::asio::mutable_buffer{p.first};
  auto peer = std::make_shared<boost::asio::ip::udp::endpoint>();
  _Socket.async_receive_from(
      buffer, *peer,
      [me = shared_from_this(), p{std::move(p)}, m, peer](const ErrorCode& ec, std::size_t bytes_transferred) mutable {
        me->_ReadPending = false;
        if (!ec) {
          assert(bytes_transferred <= p.first.Capacity - p.first.Offset);
          p.first.Length = bytes_transferred;

          auto h = m->find(*peer);
          if (h != m->end()) {
            auto handler = std::move(h->second);
            m->erase(h);
            handler(ec, std::move(p));
          } else {
            BOOST_LOG_TRIVIAL(info) << "udp(" << &*me << ") packet from unknown peer: " << *peer;
          }
        } else {
          BOOST_LOG_TRIVIAL(error) << "udp(" << &*me << ") read error: " << ec.category().name() << ':' << ec.value();
        }

        if (!m->empty()) {
          me->ScheduleRead(m);
        }
      });
}

void Udp::Write(boost::asio::ip::udp::endpoint const& peer, Packet&& p,
                std::move_only_function<WriteHandler>&& handler) {
  if (_State != kRunning) {
    return;
  }
  _WriteQueue.push(std::make_tuple(peer, std::move(p), std::move(handler)));
  ScheduleWrite();
}

void Udp::ScheduleWrite() {
  if (_State != kRunning || _WritePending || _WriteQueue.empty()) {
    return;
  }

  _WritePending = true;
  auto& next = _WriteQueue.front();
  auto peer = std::get<0>(next);
  auto p = std::move(std::get<1>(next));
  auto handler = std::move(std::get<2>(next));
  _WriteQueue.pop();

  _Socket.async_send_to(boost::asio::const_buffer{p.first}, peer,
                        [me = shared_from_this(), handler = std::move(handler)](const ErrorCode& ec,
                                                                                std::size_t bytes_transferred) mutable {
                          me->_WritePending = false;
                          handler(ec, bytes_transferred);
                          me->ScheduleWrite();
                        });
}

} // namespace gh
