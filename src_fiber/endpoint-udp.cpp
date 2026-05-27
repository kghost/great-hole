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
#include "utils.hpp"

namespace gh {

// ==================== UdpChannel ====================

Udp::UdpChannel::UdpChannel(std::shared_ptr<Udp> parent, boost::asio::ip::udp::endpoint const& peer)
    : _Parent(parent), _Peer(peer) {}

Udp::UdpChannel::~UdpChannel() { _Parent->RemoveChannel(_Peer); }

Omni::Fiber::Coroutine<ErrorCode> Udp::UdpChannel::Start(Omni::Fiber::Event<>& stopSignal) {
  co_return co_await BackgroundStart(
      "Start udp-channel:" + boost::lexical_cast<std::string>(_Peer), _IsStarted, _StartedError, stopSignal,
      [me = shared_from_this()] -> Omni::Fiber::Coroutine<ErrorCode> {
        co_return co_await me->_Parent->StartChannel(me);
      },
      [me = shared_from_this()] -> Omni::Fiber::Coroutine<void> {
        me->_Parent->StopChannel(me->_Peer);
        co_await me->_Pipe.GetProducer().Close();
        auto& fiber = co_await Omni::Fiber::GetCurrentFiber();
        co_await fiber.WaitAll();
        co_return;
      });
}

Omni::Fiber::Coroutine<ErrorCode> Udp::UdpChannel::Read(Packet& p) {
  auto [err, data] = co_await _Pipe.GetConsumer();
  if (err == decltype(err)::Data) {
    assert(data->has_value());
    auto& data2 = data.value();
    if (data2.has_value()) {
      p = std::move(data2.value());
      co_return ErrorCode{};
    } else {
      co_return data2.error();
    }
  } else if (err == decltype(err)::End) {
    p._Length = 0;
    co_return ErrorCode{AppErrorCategory::kEndOfStream, kAppError};
  } else {
    std::unreachable();
    throw std::logic_error("Unknown pipe data state");
  }
}

Omni::Fiber::Coroutine<ErrorCode> Udp::UdpChannel::Write(Packet& p) { co_return co_await _Parent->WriteTo(_Peer, p); }

// ==================== Udp ====================

Udp::Udp(boost::asio::io_context& ioContext) : _Socket(ioContext), _Local(boost::asio::ip::udp::v6(), 0) {}
Udp::Udp(boost::asio::io_context& ioContext, boost::asio::ip::udp::endpoint bind) : _Socket(ioContext), _Local(bind) {}
Udp::~Udp() {}

std::shared_ptr<Endpoint> Udp::CreateChannel(boost::asio::ip::udp::endpoint const& peer) {
  auto it = _Channels.find(peer);
  if (it != _Channels.end()) {
    if (auto ch = it->second.lock()) {
      return ch;
    }
  }
  auto ch = std::shared_ptr<UdpChannel>(new UdpChannel(shared_from_this(), peer));
  _Channels[peer] = ch;
  return ch;
}

Omni::Fiber::Coroutine<ErrorCode> Udp::StartChannel(std::shared_ptr<UdpChannel> channel) {
  _ActiveChannels[channel->GetPeer()] = channel;

  co_return co_await BackgroundStart(
      "Start udp:" + boost::lexical_cast<std::string>(_Local), _IsStarted, _StartedError, _StopSignal,
      [this]() -> Omni::Fiber::Coroutine<ErrorCode> { co_return co_await DoStart(); },
      [this]() -> Omni::Fiber::Coroutine<void> {
        _Socket.close();
        auto& fiber = co_await Omni::Fiber::GetCurrentFiber();
        co_await fiber.WaitAll();
        co_return;
      });
}

void Udp::StopChannel(boost::asio::ip::udp::endpoint const& peer) {
  _ActiveChannels.erase(peer);
  if (_ActiveChannels.empty()) {
    _StopSignal.Fire();
  }
}

void Udp::RemoveChannel(boost::asio::ip::udp::endpoint const& peer) {
  _Channels.erase(peer);
  _ActiveChannels.erase(peer);
}

Omni::Fiber::Coroutine<ErrorCode> Udp::DoStart() {
  try {
    _Socket.open(boost::asio::ip::udp::v6());
    _Socket.set_option(boost::asio::ip::v6_only(false));
    _Socket.bind(_Local);
    BOOST_LOG_TRIVIAL(info) << "udp(" << this << ") bound at " << _Socket.local_endpoint();

    auto& fiber = co_await Omni::Fiber::GetCurrentFiber();
    fiber.Spawn("udp-read-loop", [this, self = shared_from_this()]() -> Omni::Fiber::Coroutine<void> {
      while (true) {
        Packet p;
        boost::asio::ip::udp::endpoint peer;

        auto [err, bytes_transferred] =
            co_await _Socket.async_receive_from(boost::asio::mutable_buffer(p), peer, Omni::Fiber::AsioUseFiber);
        if (err) {
          BOOST_LOG_TRIVIAL(error) << "udp(" << this << ") read error: " << err.message();
          for (auto& [endpoint, ch_weak] : _ActiveChannels) {
            if (auto ch = ch_weak.lock()) {
              co_await ch->Send(std::unexpected(err));
            }
          }
          break;
        }

        p._Length = bytes_transferred;
        auto it = _ActiveChannels.find(peer);
        if (it != _ActiveChannels.end()) {
          if (auto ch = it->second.lock()) {
            co_await ch->Send(std::move(p));
          } else {
            BOOST_LOG_TRIVIAL(info) << "udp(" << this << ") packet from lost peer: " << peer;
          }
        } else {
          BOOST_LOG_TRIVIAL(info) << "udp(" << this << ") packet from unknown peer: " << peer;
        }
      }
    });

    co_return ErrorCode{};
  } catch (const SystemError& e) {
    BOOST_LOG_TRIVIAL(info) << "udp(" << this << ") start failed: " << e.what();
    co_return e.code();
  }
}

Omni::Fiber::Coroutine<ErrorCode> Udp::WriteTo(boost::asio::ip::udp::endpoint const& peer, Packet& p) {
  auto [err, bytes_transferred] =
      co_await _Socket.async_send_to(boost::asio::const_buffer(p), peer, Omni::Fiber::AsioUseFiber);
  assert(bytes_transferred == p._Length);
  co_return err;
}

} // namespace gh
