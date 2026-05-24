#include "endpoint-udp-mux-client.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/log/trivial.hpp>
#include <cassert>

namespace gh {

UdpMuxClient::UdpMuxClient(boost::asio::io_context& io_context, uint8_t id, boost::asio::ip::udp::endpoint peer)
    : _Socket(io_context), _Id(id), _Peer(peer), _Local(boost::asio::ip::udp::v6(), 0) {}
UdpMuxClient::UdpMuxClient(boost::asio::io_context& io_context, uint8_t id, boost::asio::ip::udp::endpoint peer,
                           boost::asio::ip::udp::endpoint local)
    : _Socket(io_context), _Id(id), _Peer(peer), _Local(local) {}

void UdpMuxClient::AsyncStart(std::move_only_function<Event>&& handler) {
  switch (_State) {
  case kNone:
    _State = kOpening;
    try {
      _Socket.open(boost::asio::ip::udp::v6());
      _Socket.set_option(boost::asio::ip::v6_only(false));
      _Socket.bind(_Local);
      BOOST_LOG_TRIVIAL(info) << "UdpMuxClient(" << this << ") bound at " << _Socket.local_endpoint();
    } catch (const SystemError& e) {
      BOOST_LOG_TRIVIAL(info) << "UdpMuxClient(" << this << ") start failed: " << e.what();
      _State = kError;
      handler(e.code());
      return;
    }

    _Socket.async_connect(_Peer, [me = shared_from_this(), handler{std::move(handler)}](const ErrorCode& ec) mutable {
      if (!ec) {
        me->_State = kRunning;
      }
      handler(ErrorCode());
    });
    break;
  case kOpening:
    BOOST_LOG_TRIVIAL(info) << "UdpMuxClient(" << this << ") start opening";
    handler(ErrorCode());
    break;
  case kRunning:
    handler(ErrorCode());
    break;
  default:
    handler(ErrorCode{AppErrorCategory::kIncorrectState, kAppError});
    break;
  }
}

void UdpMuxClient::AsyncRead(std::move_only_function<ReadHandler>&& handler) {
  auto a = std::make_shared<std::array<uint8_t, 2048>>();
  auto p = Packet{Buffer(*a), a};
  auto buffer = boost::asio::mutable_buffer{p.first};
  _Socket.async_receive(buffer, [me = shared_from_this(), p{std::move(p)}, handler{std::move(handler)}](
                                    const ErrorCode& ec, std::size_t bytes_transferred) mutable {
    if (!ec) {
      auto& buf = p.first;

      assert(bytes_transferred <= buf.Capacity - buf.Offset);
      buf.Length = bytes_transferred;

      if (bytes_transferred < 1) {
        BOOST_LOG_TRIVIAL(info) << "UdpMuxClient(" << &*me << ") ignored empty packet";
        me->AsyncRead(std::move(handler));
        return;
      }

      uint8_t channel_id = buf.Data[buf.Offset];
      if (channel_id != me->_Id) {
        BOOST_LOG_TRIVIAL(info) << "UdpMuxClient(" << &*me
                                << ") ignored packet for unknown channel: " << (int)channel_id;
        me->AsyncRead(std::move(handler));
        return;
      }

      buf.Offset += 1;
      buf.Length -= 1;
    }
    handler(ec, std::move(p));
  });
}

void UdpMuxClient::AsyncWrite(Packet&& p, std::move_only_function<WriteHandler>&& handler) {
  if (_State != kRunning) {
    return;
  }
  assert(!_WritePending);
  _WritePending = true;

  auto& buf = p.first;
  if (buf.Offset < 1) {
    handler(ErrorCode{AppErrorCategory::kInvalidPacketReserved, kAppError}, 0);
    _WritePending = false;
    return;
  }

  buf.Offset -= 1;
  buf.Length += 1;
  buf.Data[buf.Offset] = _Id;

  _Socket.async_send(boost::asio::const_buffer{buf},
                     [me = shared_from_this(), p = std::move(p),
                      handler = std::move(handler)](const ErrorCode& ec, std::size_t bytes_transferred) mutable {
                       me->_WritePending = false;
                       handler(ec, bytes_transferred);
                     });
}

} // namespace gh
