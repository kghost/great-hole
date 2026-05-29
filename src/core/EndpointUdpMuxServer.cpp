#include "EndpointUdpMuxServer.hpp"

#include <cassert>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <memory>
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

// ==================== UdpMuxServer::Channel ====================

UdpMuxServer::Channel::Channel(std::shared_ptr<UdpMuxServer> parent, uint8_t id) : _Parent(parent), _Id(id) {}

UdpMuxServer::Channel::Channel(std::shared_ptr<UdpMuxServer> parent, uint8_t id, std::shared_ptr<ResolverEndpoint> peer)
    : _Parent(parent), _Id(id), _PeerResolver(peer) {}

UdpMuxServer::Channel::~Channel() { _Parent->RemoveChannel(_Id); }

std::string UdpMuxServer::Channel::GetName() const { return std::format("UdpMuxChannel:[{}]", _Id); }

Omni::Fiber::Coroutine<ErrorCode> UdpMuxServer::Channel::DoStart() {
  if (_PeerResolver) {
    auto err = co_await _PeerResolver->Start();
    if (err) {
      co_await (co_await Omni::Fiber::GetCurrentFiber()).WaitAll();
      co_return err;
    }
    auto peerEp = _PeerResolver->GetEndpoint();
    co_await _PeerResolver->Stop();
    co_await (co_await Omni::Fiber::GetCurrentFiber()).WaitAll();

    auto it = _Parent->_Channels.find(_Id);
    if (it != _Parent->_Channels.end()) {
      it->second.Peer = peerEp;
    }
  }
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> UdpMuxServer::Channel::DoGracefulStop() {
  co_await _PipielineUsageCounter.WaitAll();
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> UdpMuxServer::Channel::Read(Packet& p, Cancel& c) {
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

Omni::Fiber::Coroutine<ErrorCode> UdpMuxServer::Channel::Write(Packet& p, Cancel& c) {
  co_return co_await _Parent->WriteTo(_Id, p, c);
}

// ==================== UdpMuxServer ====================

UdpMuxServer::UdpMuxServer(boost::asio::io_context& ioContext)
    : _Socket(ioContext), _Local(boost::asio::ip::udp::v6(), 0) {}

UdpMuxServer::UdpMuxServer(boost::asio::io_context& ioContext, boost::asio::ip::udp::endpoint bind)
    : _Socket(ioContext), _Local(bind) {}

UdpMuxServer::~UdpMuxServer() {}

std::string UdpMuxServer::GetName() const { return "UdpMuxServer:" + boost::lexical_cast<std::string>(_Local); }

Omni::Fiber::Coroutine<ErrorCode> UdpMuxServer::DoStart() {
  try {
    _Socket.open(boost::asio::ip::udp::v6());
    _Socket.set_option(boost::asio::ip::v6_only(false));
    _Socket.bind(_Local);
    BOOST_LOG_TRIVIAL(info) << "UdpMuxServer(" << this << ") bound at " << _Socket.local_endpoint();
  } catch (const SystemError& e) {
    BOOST_LOG_TRIVIAL(info) << "UdpMuxServer(" << this << ") start failed: " << e.what();
    co_return e.code();
  }
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<void> UdpMuxServer::DoWork() {
  (co_await Omni::Fiber::GetCurrentFiber())
      .Spawn("UdpMuxServer ReadLoop:" + boost::lexical_cast<std::string>(LocalEndpoint()) + "@" +
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

Omni::Fiber::Coroutine<ErrorCode> UdpMuxServer::DoGracefulStop() {
  for (auto& [id, info] : _Channels) {
    if (auto ch = info.WeakChannel.lock()) {
      co_await ch->Stop();
    }
  }
  co_await (co_await Omni::Fiber::GetCurrentFiber()).WaitAll();
  _Socket.close();
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<std::shared_ptr<Endpoint>> UdpMuxServer::CreateChannel(uint8_t id) {
  auto ch = std::make_shared<Channel>(std::static_pointer_cast<UdpMuxServer>(shared_from_this()), id);
  auto [it, inserted] = _Channels.try_emplace(id, ChannelInfo{.WeakChannel = ch, .Peer = std::nullopt});
  assert(inserted);

  co_await _CreateChannelPipe.GetProducer().Put([this, id, ch]() -> Omni::Fiber::Coroutine<void> {
    auto err = co_await ch->Start();
    if (err) {
      BOOST_LOG_TRIVIAL(error) << "UdpMuxServer Channel start failed: " << err.message();
    }
    co_return;
  });

  co_return ch;
}

Omni::Fiber::Coroutine<std::shared_ptr<Endpoint>> UdpMuxServer::CreateChannel(uint8_t id,
                                                                              std::shared_ptr<ResolverEndpoint> peer) {
  auto ch = std::make_shared<Channel>(std::static_pointer_cast<UdpMuxServer>(shared_from_this()), id, peer);
  auto [it, inserted] = _Channels.try_emplace(id, ChannelInfo{.WeakChannel = ch, .Peer = std::nullopt});
  assert(inserted);

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

void UdpMuxServer::RemoveChannel(uint8_t id) { _Channels.erase(id); }

Omni::Fiber::Coroutine<void> UdpMuxServer::ReadLoop() {
  auto slotTracker = _Stop.AsioSlot();
  while (!_Stop.IsTriggered()) {
    Packet p;
    boost::asio::ip::udp::endpoint peer;

    auto [err, bytes_transferred] = co_await _Socket.async_receive_from(
        boost::asio::mutable_buffer(p), peer,
        boost::asio::bind_cancellation_slot(slotTracker.Slot(), Omni::Fiber::AsioUseFiber));
    if (err) {
      if (err == boost::asio::error::operation_aborted) {
        BOOST_LOG_TRIVIAL(info) << "UdpMuxServer(" << this << ") read loop cancelled";
      } else {
        BOOST_LOG_TRIVIAL(error) << "UdpMuxServer(" << this << ") read error: " << err.message();
        for (auto& [id, info] : _Channels) {
          if (auto ch = info.WeakChannel.lock()) {
            if (!ch->IsStopped()) {
              co_await ch->Send(std::unexpected(err));
            }
          }
        }
      }
      break;
    }
    p._Length = bytes_transferred;

    if (p.DataSize() < 1) {
      BOOST_LOG_TRIVIAL(info) << "UdpMuxServer(" << this << ") ignored empty packet from " << peer;
      continue;
    }

    uint8_t id = p.PopFront(1)[0];
    auto it = _Channels.find(id);
    if (it != _Channels.end()) {
      if (it->second.Peer.has_value()) {
        if (it->second.Peer.value() != peer) {
          BOOST_LOG_TRIVIAL(info) << GetName() << " remaps peer " << it->second.Peer.value() << " to " << peer
                                  << " for channel " << (int)id;
          it->second.Peer = peer;
        }
      } else {
        BOOST_LOG_TRIVIAL(info) << GetName() << " learns peer " << peer << " for channel " << (int)id;
        it->second.Peer = peer;
      }

      if (auto ch = it->second.WeakChannel.lock()) {
        co_await ch->Send(std::move(p));
      } else {
        BOOST_LOG_TRIVIAL(info) << "UdpMuxServer(" << this << ") packet from lost channel ID: " << (int)id;
      }
    } else {
      BOOST_LOG_TRIVIAL(info) << "UdpMuxServer(" << this << ") packet from unknown channel ID: " << (int)id;
    }
  }
}

Omni::Fiber::Coroutine<ErrorCode> UdpMuxServer::WriteTo(uint8_t id, Packet& p, Cancel& c) {
  if (c.IsTriggered()) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }

  auto it = _Channels.find(id);
  if (it == _Channels.end() || !it->second.Peer.has_value()) {
    co_return ErrorCode{AppErrorCategory::kInvalidPacketSession, kAppError};
  }

  if (p.FrontSpace() < 1) {
    co_return ErrorCode{AppErrorCategory::kInvalidPacketReserved, kAppError};
  }

  p.PushFront(std::span<uint8_t, 1>(&id, 1));

  auto [err, bytes_transferred] = co_await _Socket.async_send_to(
      boost::asio::const_buffer(p), it->second.Peer.value(),
      boost::asio::bind_cancellation_slot(c.AsioSlot().Slot(), Omni::Fiber::AsioUseFiber));
  assert(err || bytes_transferred == p._Length);
  co_return err;
}

} // namespace gh
