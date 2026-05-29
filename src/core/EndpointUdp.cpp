#include "EndpointUdp.hpp"

#include <cassert>
#include <expected>
#include <map>
#include <memory>
#include <tuple>
#include <utility>

#include <boost/asio/buffer.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "ErrorCode.hpp"
#include "GetCurrentFiber.hpp"
#include "Select.hpp"

namespace gh {

// ==================== UdpChannel ====================

Udp::UdpChannel::UdpChannel(std::shared_ptr<Udp> parent, UdpChannel::Target target) : _Parent(parent), _Peer(target) {}
Udp::UdpChannel::~UdpChannel() {
  if (std::holds_alternative<boost::asio::ip::udp::endpoint>(_Peer)) {
    _Parent->RemoveChannel(std::get<boost::asio::ip::udp::endpoint>(_Peer));
  }
}

std::string Udp::UdpChannel::GetName() const {
  std::string ret = "UdpChannel:";
  if (std::holds_alternative<boost::asio::ip::udp::endpoint>(_Peer)) {
    ret += boost::lexical_cast<std::string>(std::get<boost::asio::ip::udp::endpoint>(_Peer));
  } else if (std::holds_alternative<std::shared_ptr<ResolverEndpoint>>(_Peer)) {
    ret += "@" + std::get<std::shared_ptr<ResolverEndpoint>>(_Peer)->GetName();
  }
  return ret;
}

Omni::Fiber::Coroutine<ErrorCode> Udp::UdpChannel::DoStart() {
  if (std::holds_alternative<std::shared_ptr<ResolverEndpoint>>(_Peer)) {
    auto resolver = std::get<std::shared_ptr<ResolverEndpoint>>(_Peer);
    auto err = co_await resolver->Start();
    if (err) {
      co_await (co_await Omni::Fiber::GetCurrentFiber()).WaitAll();
      co_return err;
    }
    _Peer = resolver->GetEndpoint();
    co_await resolver->Stop();
    co_await (co_await Omni::Fiber::GetCurrentFiber()).WaitAll();
  }
  _Parent->AddChannel(std::get<boost::asio::ip::udp::endpoint>(_Peer),
                      std::dynamic_pointer_cast<UdpChannel>(shared_from_this()));
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> Udp::UdpChannel::DoGracefulStop() {
  co_await _PipielineUsageCounter.WaitAll();
  co_await (co_await Omni::Fiber::GetCurrentFiber()).WaitAll();
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
  co_return co_await _Parent->WriteTo(std::get<boost::asio::ip::udp::endpoint>(_Peer), p, c);
}

// ==================== Udp ====================
Udp::Udp(boost::asio::io_context& ioContext) : _Socket(ioContext), _Local(boost::asio::ip::udp::v6(), 0) {}
Udp::Udp(boost::asio::io_context& ioContext, boost::asio::ip::udp::endpoint bind) : _Socket(ioContext), _Local(bind) {}
Udp::~Udp() {}

std::string Udp::GetName() const { return "Udp:" + boost::lexical_cast<std::string>(_Local); }

Omni::Fiber::Coroutine<ErrorCode> Udp::DoStart() {
  try {
    _Socket.open(boost::asio::ip::udp::v6());
    _Socket.set_option(boost::asio::ip::v6_only(false));
    _Socket.bind(_Local);
    BOOST_LOG_TRIVIAL(info) << "udp(" << this << ") bound at " << _Socket.local_endpoint();
  } catch (const SystemError& e) {
    BOOST_LOG_TRIVIAL(info) << "udp(" << this << ") start failed: " << e.what();
    co_return e.code();
  }
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<void> Udp::DoWork() {
  auto& currentFiber = co_await Omni::Fiber::GetCurrentFiber();
  currentFiber.Spawn("Udp ReadLoop:" + boost::lexical_cast<std::string>(LocalEndpoint()) + "@" +
                         std::to_string(reinterpret_cast<uintptr_t>(this)),
                     [this]() -> Omni::Fiber::Coroutine<void> {
                       co_await ReadLoop();
                       co_return;
                     });

  bool stopped = false;
  while (!stopped) {
    std::optional<std::move_only_function<Omni::Fiber::Coroutine<void>()>> pendingFunc;
    co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(_Stop.GetFiberCancelEvent(), [&]() { stopped = true; }),
                                 Omni::Fiber::SelectPair(_CreateChannelPipe.GetConsumer(), [&](auto data) {
                                   if (data.has_value()) {
                                     pendingFunc = std::move(data.value());
                                   } else {
                                     stopped = true;
                                   }
                                 }));
    if (pendingFunc.has_value()) {
      co_await pendingFunc.value()();
    }
  }
}

Omni::Fiber::Coroutine<ErrorCode> Udp::DoGracefulStop() {
  for (auto& [peer, ch_weak] : _Channels) {
    if (auto ch = ch_weak.lock()) {
      co_await ch->Stop();
    }
  }
  auto& currentFiber = co_await Omni::Fiber::GetCurrentFiber();
  co_await currentFiber.WaitAll();
  _Socket.close();
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<std::shared_ptr<Endpoint>> Udp::CreateChannel(std::shared_ptr<ResolverEndpoint> resolver) {
  auto ch = std::make_shared<UdpChannel>(std::static_pointer_cast<Udp>(shared_from_this()), resolver);

  auto event = std::make_shared<Omni::Fiber::Event<ErrorCode>>();
  co_await _CreateChannelPipe.GetProducer().Put([ch, event]() -> Omni::Fiber::Coroutine<void> {
    auto err = co_await ch->Start();
    event->Fire(err);
    co_return;
  });

  auto err = co_await *event;
  if (err) {
    co_return nullptr;
  }
  co_return ch;
}

Omni::Fiber::Coroutine<std::shared_ptr<Endpoint>> Udp::CreateChannel(boost::asio::ip::udp::endpoint const& peer) {
  auto ch = std::make_shared<UdpChannel>(std::static_pointer_cast<Udp>(shared_from_this()), peer);

  auto event = std::make_shared<Omni::Fiber::Event<ErrorCode>>();
  co_await _CreateChannelPipe.GetProducer().Put([ch, event]() -> Omni::Fiber::Coroutine<void> {
    auto err = co_await ch->Start();
    event->Fire(err);
    co_return;
  });

  auto err = co_await *event;
  if (err) {
    co_return nullptr;
  }
  co_return ch;
}

void Udp::RemoveChannel(boost::asio::ip::udp::endpoint const& peer) { _Channels.erase(peer); }

void Udp::AddChannel(boost::asio::ip::udp::endpoint const& peer, std::shared_ptr<UdpChannel> ch) {
  assert(!_Channels.contains(peer));
  _Channels[peer] = ch;
}

Omni::Fiber::Coroutine<void> Udp::ReadLoop() {
  auto slotTracker = _Stop.AsioSlot();
  while (!_Stop.IsTriggered()) {
    Packet p;
    boost::asio::ip::udp::endpoint peer;

    auto [err, bytes_transferred] = co_await _Socket.async_receive_from(
        boost::asio::mutable_buffer(p), peer,
        boost::asio::bind_cancellation_slot(slotTracker.Slot(), Omni::Fiber::AsioUseFiber));
    if (err) {
      if (err == boost::asio::error::operation_aborted) {
        BOOST_LOG_TRIVIAL(info) << "udp(" << this << ") read loop cancelled";
      } else {
        BOOST_LOG_TRIVIAL(error) << "udp(" << this << ") read error: " << err.message();
        for (auto& [endpoint, ch_weak] : _Channels) {
          if (auto ch = ch_weak.lock()) {
            if (!ch->IsStopped()) {
              co_await ch->Send(std::unexpected(err));
            }
          }
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
      boost::asio::bind_cancellation_slot(c.AsioSlot().Slot(), Omni::Fiber::AsioUseFiber));
  assert(err || bytes_transferred == p._Length);
  co_return err;
}

} // namespace gh
