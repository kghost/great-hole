#include "EndpointUdpMuxClient.hpp"

#include <cassert>
#include <string>
#include <utility>

#include <boost/asio/buffer.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "ErrorCode.hpp"

namespace gh {

UdpMuxClient::UdpMuxClient(boost::asio::io_context& ioContext, uint8_t id, boost::asio::ip::udp::endpoint peer)
    : _Socket(ioContext), _Id(id), _Peer(peer), _Local(boost::asio::ip::udp::v6(), 0) {}

UdpMuxClient::UdpMuxClient(boost::asio::io_context& ioContext, uint8_t id, boost::asio::ip::udp::endpoint peer,
                           boost::asio::ip::udp::endpoint local)
    : _Socket(ioContext), _Id(id), _Peer(peer), _Local(local) {}

UdpMuxClient::~UdpMuxClient() {}

std::string UdpMuxClient::GetName() const { return "UdpMuxClient:" + std::to_string(_Id); }

Omni::Fiber::Coroutine<ErrorCode> UdpMuxClient::DoStart() {
  try {
    _Socket.open(boost::asio::ip::udp::v6());
    _Socket.set_option(boost::asio::ip::v6_only(false));
    _Socket.bind(_Local);
    BOOST_LOG_TRIVIAL(info) << "UdpMuxClient(" << this << ") bound at " << _Socket.local_endpoint();
  } catch (const SystemError& e) {
    BOOST_LOG_TRIVIAL(info) << "UdpMuxClient(" << this << ") start failed on bind: " << e.what();
    co_return e.code();
  }

  auto [err] = co_await _Socket.async_connect(
      _Peer, boost::asio::bind_cancellation_slot(_Stop.GetAsioCancelSlot(), Omni::Fiber::AsioUseFiber));
  if (err) {
    BOOST_LOG_TRIVIAL(info) << "UdpMuxClient(" << this << ") connect failed: " << err.message();
    co_return err;
  }

  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> UdpMuxClient::DoGracefulStop() {
  co_await _PipielineUsageCounter.WaitAll();
  _Socket.close();
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> UdpMuxClient::Read(Packet& p, Cancel& c) {
  while (true) {
    if (c.IsTriggered()) {
      co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
    }

    auto [err, bytes_transferred] = co_await _Socket.async_receive(
        boost::asio::mutable_buffer(p),
        boost::asio::bind_cancellation_slot(c.GetAsioCancelSlot(), Omni::Fiber::AsioUseFiber));
    if (err) {
      co_return err;
    }

    if (bytes_transferred < 1) {
      BOOST_LOG_TRIVIAL(info) << "UdpMuxClient(" << this << ") ignored empty packet";
      continue;
    }

    uint8_t channel_id = p._Data[p._Offset];
    if (channel_id != _Id) {
      BOOST_LOG_TRIVIAL(info) << "UdpMuxClient(" << this << ") ignored packet for unknown channel: " << (int)channel_id;
      continue;
    }

    p._Offset += 1;
    p._Length = bytes_transferred - 1;
    co_return ErrorCode{};
  }
}

Omni::Fiber::Coroutine<ErrorCode> UdpMuxClient::Write(Packet& p, Cancel& c) {
  if (c.IsTriggered()) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }

  if (p._Offset < 1) {
    co_return ErrorCode{AppErrorCategory::kInvalidPacketReserved, kAppError};
  }

  p._Offset -= 1;
  p._Length += 1;
  p._Data[p._Offset] = _Id;

  auto [err, bytes_transferred] = co_await _Socket.async_send(
      boost::asio::const_buffer(p),
      boost::asio::bind_cancellation_slot(c.GetAsioCancelSlot(), Omni::Fiber::AsioUseFiber));
  assert(err || bytes_transferred == p._Length);
  co_return err;
}

} // namespace gh
