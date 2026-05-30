#include "EndpointUdp.hpp"

#include <cassert>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <tuple>
#include <utility>

#include <boost/asio/buffer.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "GetCurrentFiber.hpp"
#include "RemoteCall.hpp"
#include "ResolverStaticEndpoint.hpp"
#include "Select.hpp"

namespace gh {

// ==================== Udp ====================
Udp::Udp(boost::asio::io_context& ioContext) : _Socket(ioContext), _Local(boost::asio::ip::udp::v6(), 0) {}
Udp::Udp(boost::asio::io_context& ioContext, boost::asio::ip::udp::endpoint bind) : _Socket(ioContext), _Local(bind) {}
Udp::~Udp() { assert(_Channels.empty()); }

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
  (co_await Omni::Fiber::GetCurrentFiber())
      .Spawn("Udp ReadLoop:" + boost::lexical_cast<std::string>(LocalEndpoint()) + "@" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)),
             [this]() -> Omni::Fiber::Coroutine<void> {
               co_await ReadLoop();
               co_return;
             });

  bool stopped = false;
  while (!stopped) {
    co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(_Stop.GetFiberCancelEvent(), [&]() { stopped = true; }),
        Omni::Fiber::SelectPair(_ChannelRpc.GetServiceAwaitor(), [&](auto req) -> Omni::Fiber::Coroutine<void> {
          bool ok = co_await Omni::Fiber::RemoteCall::HandleRequest(std::move(req));
          assert(ok);
          co_return;
        }));
  }
}

Omni::Fiber::Coroutine<ErrorCode> Udp::DoGracefulStop() {
  for (auto& [peer, channel] : std::exchange(_Channels, {})) {
    co_await channel->Stop();
    co_await channel->WaitService();
  }
  co_await (co_await Omni::Fiber::GetCurrentFiber()).WaitAll();
  _Socket.close();
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<std::shared_ptr<Udp::UdpChannel>>
Udp::CreateChannel(std::shared_ptr<ResolverEndpoint> resolver) {
  co_return co_await _ChannelRpc.Call(
      [this, resolver](this auto self) -> Omni::Fiber::Coroutine<std::shared_ptr<Udp::UdpChannel>> {
        auto peer = co_await resolver->Resolve();
        if (!peer.has_value()) {
          co_return nullptr;
        }

        auto ep = peer.value();
        auto channel = std::make_shared<UdpChannel>(*this, ep);
        auto [it, inserted] = _Channels.try_emplace(ep, channel);
        assert(inserted);
        auto err = co_await channel->Start();
        if (err) {
          co_return nullptr;
        }
        co_return channel;
      });
}

Omni::Fiber::Coroutine<std::shared_ptr<Udp::UdpChannel>>
Udp::CreateChannel(boost::asio::ip::udp::endpoint const& peer) {
  co_return co_await CreateChannel(std::make_shared<ResolverStaticEndpoint>(peer));
}

Omni::Fiber::Coroutine<void> Udp::RemoveChannel(const boost::asio::ip::udp::endpoint& peer) {
  co_await _ChannelRpc.Call([this, peer]() -> Omni::Fiber::Coroutine<void> {
    auto it = _Channels.find(peer);
    assert(it != _Channels.end());

    auto channel = std::move(it->second);
    _Channels.erase(it);
    co_await channel->Stop();
    co_await channel->WaitService();
  });
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
        for (auto& [endpoint, channel] : _Channels) {
          if (!channel->IsStopped()) {
            co_await channel->Send(std::unexpected(err));
          }
        }
      }
      break;
    }

    p._Length = bytes_transferred;
    auto it = _Channels.find(peer);
    if (it != _Channels.end()) {
      co_await it->second->Send(std::move(p));
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

// ==================== UdpChannel ====================
Udp::UdpChannel::UdpChannel(Udp& parent, boost::asio::ip::udp::endpoint peer) : _Parent(parent), _Peer(peer) {}
Udp::UdpChannel::~UdpChannel() {}

std::string Udp::UdpChannel::GetName() const {
  return std::format("UdpChannel({:p})@({}):{}", static_cast<const void*>(this),
                     boost::lexical_cast<std::string>(_Parent.LocalEndpoint()),
                     boost::lexical_cast<std::string>(_Peer));
}

Omni::Fiber::Coroutine<ErrorCode> Udp::UdpChannel::DoStart() { co_return ErrorCode{}; }

Omni::Fiber::Coroutine<ErrorCode> Udp::UdpChannel::DoGracefulStop() {
  co_await _PipielineUsageCounter.WaitAll();
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
  co_return co_await _Parent.WriteTo(_Peer, p, c);
}

} // namespace gh
