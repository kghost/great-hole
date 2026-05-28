#include "endpoint-udp.hpp"

#include <cassert>
#include <expected>
#include <map>
#include <memory>
#include <tuple>

#include <boost/asio/buffer.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>
#include <utility>

#include "Asio.hpp"
#include "GetCurrentFiber.hpp"
#include "error-code.hpp"

namespace gh {

// ==================== UdpChannel ====================

Udp::UdpChannel::UdpChannel(std::shared_ptr<Udp> parent, boost::asio::ip::udp::endpoint const& peer)
    : _Parent(parent), _Peer(peer) {}
Udp::UdpChannel::~UdpChannel() { _Parent->RemoveChannel(_Peer); }

Omni::Fiber::Coroutine<ErrorCode> Udp::UdpChannel::Start(Omni::Fiber::Event<>& stopSignal) { co_return ErrorCode{}; }

Omni::Fiber::Coroutine<ErrorCode> Udp::UdpChannel::Read(Packet& p) {
  auto data = co_await _Pipe.GetConsumer();
  if (data.has_value()) {
    auto& inner = data.value();
    if (inner.has_value()) {
      p = std::move(inner.value());
      co_return ErrorCode{};
    } else {
      co_return inner.error();
    }
  } else {
    p._Length = 0;
    co_return ErrorCode{AppErrorCategory::kEndOfStream, kAppError};
  }
}

Omni::Fiber::Coroutine<ErrorCode> Udp::UdpChannel::Write(Packet& p) { co_return co_await _Parent->WriteTo(_Peer, p); }

// ==================== Udp ====================
Udp::Udp(boost::asio::io_context& ioContext) : _Socket(ioContext), _Local(boost::asio::ip::udp::v6(), 0) {}
Udp::Udp(boost::asio::io_context& ioContext, boost::asio::ip::udp::endpoint bind) : _Socket(ioContext), _Local(bind) {}
Udp::~Udp() {}

Omni::Fiber::Coroutine<ErrorCode> Udp::Start(Omni::Fiber::Event<>& stopSignal) {
  Omni::Fiber::Event<ErrorCode> start_error;
  auto& fiber = co_await Omni::Fiber::GetCurrentFiber();
  fiber.Spawn("Start udp-channel:" + boost::lexical_cast<std::string>(_Local),
              [this, &start_error]() -> Omni::Fiber::Coroutine<void> {
                auto me = shared_from_this(); // Hold me to prevent this from releasing.
                try {
                  _Socket.open(boost::asio::ip::udp::v6());
                  _Socket.set_option(boost::asio::ip::v6_only(false));
                  _Socket.bind(_Local);
                  BOOST_LOG_TRIVIAL(info) << "udp(" << this << ") bound at " << _Socket.local_endpoint();
                  start_error.Fire(ErrorCode{});
                  // Start successful

                  co_await ReadLoop();

                  co_return;
                } catch (const SystemError& e) {
                  BOOST_LOG_TRIVIAL(info) << "udp(" << this << ") start failed: " << e.what();
                  start_error.Fire(e.code());
                  co_return;
                }
              });
  co_return co_await start_error;
}

std::shared_ptr<Endpoint> Udp::CreateChannel(boost::asio::ip::udp::endpoint const& peer) {
  assert(!_Channels.contains(peer));
  auto ch = std::make_shared<UdpChannel>(shared_from_this(), peer);
  _Channels[peer] = ch;
  // FIXME: Channel do not need to start.
  return ch;
}

void Udp::RemoveChannel(boost::asio::ip::udp::endpoint const& peer) { _Channels.erase(peer); }

Omni::Fiber::Coroutine<void> Udp::ReadLoop() {
  while (true) {
    Packet p;
    boost::asio::ip::udp::endpoint peer;

    auto [err, bytes_transferred] =
        co_await _Socket.async_receive_from(boost::asio::mutable_buffer(p), peer, Omni::Fiber::AsioUseFiber);
    if (err) {
      BOOST_LOG_TRIVIAL(error) << "udp(" << this << ") read error: " << err.message();
      for (auto& [endpoint, ch_weak] : _Channels) {
        if (auto ch = ch_weak.lock()) {
          co_await ch->Send(std::unexpected(err));
        }
      }
      break;
    }

    p._Length = bytes_transferred;
    auto it = _Channels.find(peer);
    if (it != _Channels.end()) {
      if (auto ch = it->second.lock()) {
        co_await ch->Send(std::move(p));
      } else {
        BOOST_LOG_TRIVIAL(info) << "udp(" << this << ") packet from lost peer: " << peer;
      }
    } else {
      BOOST_LOG_TRIVIAL(info) << "udp(" << this << ") packet from unknown peer: " << peer;
    }
  }
}

Omni::Fiber::Coroutine<ErrorCode> Udp::WriteTo(boost::asio::ip::udp::endpoint const& peer, Packet& p) {
  auto [err, bytes_transferred] =
      co_await _Socket.async_send_to(boost::asio::const_buffer(p), peer, Omni::Fiber::AsioUseFiber);
  assert(bytes_transferred == p._Length);
  co_return err;
}

} // namespace gh
