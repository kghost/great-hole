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
#include "Cancel.hpp"
#include "GetCurrentFiber.hpp"
#include "Select.hpp"
#include "error-code.hpp"

namespace gh {

// ==================== UdpChannel ====================

Udp::UdpChannel::UdpChannel(std::shared_ptr<Udp> parent, boost::asio::ip::udp::endpoint const& peer)
    : _Parent(parent), _Peer(peer) {}
Udp::UdpChannel::~UdpChannel() { _Parent->RemoveChannel(_Peer); }

Omni::Fiber::Coroutine<ErrorCode> Udp::UdpChannel::Start() {
  auto& fiber = co_await Omni::Fiber::GetCurrentFiber();
  fiber.Spawn("Start UdpChannel:" + boost::lexical_cast<std::string>(_Peer), [this]() -> Omni::Fiber::Coroutine<void> {
    auto me = std::dynamic_pointer_cast<UdpChannel>(shared_from_this());
    co_await _Stop.GetFiberCancelEvent();
    if (_PipelineCount > 0) {
      co_await _GracefulExitEvent;
    }
    BOOST_LOG_TRIVIAL(info) << "UdpChannel(" << this << ") graceful exit triggered, closing pipe";
    _Pipe.GetProducer().Close();
    co_return;
  });
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> Udp::UdpChannel::Read(Packet& p, Cancel& c) {
  bool stopped = false;
  std::optional<ErrorCode> err;
  co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(c.GetFiberCancelEvent(), [&]() { stopped = true; }),
                               Omni::Fiber::SelectPair(_Pipe.GetConsumer(), [&](auto data) {
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

Omni::Fiber::Coroutine<ErrorCode> Udp::UdpChannel::Write(Packet& p, Cancel& c) {
  co_return co_await _Parent->WriteTo(_Peer, p, c);
}

Omni::Fiber::Coroutine<ErrorCode> Udp::UdpChannel::Stop() {
  _Stop.Trigger();
  co_return ErrorCode{};
}

// ==================== Udp ====================
Udp::Udp(boost::asio::io_context& ioContext) : _Socket(ioContext), _Local(boost::asio::ip::udp::v6(), 0) {}
Udp::Udp(boost::asio::io_context& ioContext, boost::asio::ip::udp::endpoint bind) : _Socket(ioContext), _Local(bind) {}
Udp::~Udp() {}

Omni::Fiber::Coroutine<ErrorCode> Udp::Start() {
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

Omni::Fiber::Coroutine<ErrorCode> Udp::Stop() {
  boost::system::error_code ec;
  co_return _Socket.close(ec);
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

Omni::Fiber::Coroutine<ErrorCode> Udp::WriteTo(boost::asio::ip::udp::endpoint const& peer, Packet& p, Cancel& c) {
  if (c.IsTriggered()) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }
  auto [err, bytes_transferred] = co_await _Socket.async_send_to(
      boost::asio::const_buffer(p), peer,
      boost::asio::bind_cancellation_slot(c.GetAsioCancelSlot(), Omni::Fiber::AsioUseFiber));
  assert(err || bytes_transferred == p._Length);
  co_return err;
}

} // namespace gh
