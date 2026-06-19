#include "EndpointUdpMux.hpp"

#include <cassert>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <utility>

#include <boost/asio/buffer.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>

#include "Cancel.hpp"
#include "ErrorCode.hpp"
#include "GetCurrentFiber.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"
#include "ServiceBase.hpp"

namespace gh {

// ==================== UdpMux ====================
UdpMux::UdpMux(boost::asio::any_io_executor executor) : _Socket(executor), _Local(boost::asio::ip::udp::v6(), 0) {}

UdpMux::UdpMux(boost::asio::any_io_executor executor, boost::asio::ip::udp::endpoint bind)
    : _Socket(executor), _Local(bind) {}

UdpMux::~UdpMux() { assert(_Channels.empty()); }

std::string UdpMux::GetName() const { return "UdpMux:" + boost::lexical_cast<std::string>(_Local); }

Omni::Fiber::Coroutine<ErrorCode> UdpMux::DoStart() {
  ErrorCode ec;
  try {
    _Socket.open(boost::asio::ip::udp::v6());
    _Socket.set_option(boost::asio::ip::v6_only(false));
    _Socket.bind(_Local);
    BOOST_LOG_TRIVIAL(info) << "UdpMux(" << this << ") bound at " << _Socket.local_endpoint();
  } catch (const SystemError& e) {
    BOOST_LOG_TRIVIAL(info) << "UdpMux(" << this << ") start failed: " << e.what();
    ec = e.code();
  }

  if (ec) {
    co_await _ChannelRpc.Close();
    co_return ec;
  }
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<void> UdpMux::DoWork() {
  _ReadLoopFiber = (co_await Omni::Fiber::GetCurrentFiber())
                       .Spawn("UdpMux ReadLoop:" + boost::lexical_cast<std::string>(LocalEndpoint()) + "@" +
                                  std::to_string(reinterpret_cast<uintptr_t>(this)),
                              [this]() -> Omni::Fiber::Coroutine<void> {
                                co_await ReadLoop();
                                co_return;
                              });

  bool stopped = false;
  while (!stopped) {
    auto [stopResult, rpcResult] = co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(), [] {}),
        Omni::Fiber::SelectPair(_ChannelRpc.GetServiceAwaitor(), Omni::Fiber::RemoteCall::HandleRequest));
    if (stopResult) {
      stopped = true;
    }
    if (rpcResult.has_value() && !rpcResult.value()) {
      stopped = true;
    }
  }
}

Omni::Fiber::Coroutine<ErrorCode> UdpMux::DoGracefulStop() {
  co_await _ChannelRpc.Close();
  for (auto& [id, channel] : std::exchange(_Channels, {})) {
    co_await channel->Stop();
    co_await channel->WaitService();
  }
  if (_ReadLoopFiber) {
    co_await (co_await Omni::Fiber::GetCurrentFiber()).Join(_ReadLoopFiber);
    _ReadLoopFiber.reset();
  }
  _Socket.close();
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<std::shared_ptr<UdpMux::Channel>> UdpMux::CreateChannel(uint8_t id) {
  co_return co_await CreateChannel(id, nullptr);
}

Omni::Fiber::Coroutine<std::shared_ptr<UdpMux::Channel>>
UdpMux::CreateChannel(uint8_t id, std::shared_ptr<ResolverEndpoint> resolver) {
  auto reply = co_await _ChannelRpc.Call(
      [&udp = *this, id, resolver](this auto self) -> Omni::Fiber::Coroutine<std::shared_ptr<Channel>> {
        std::shared_ptr<Channel> channel;
        if (resolver) {
          auto peer = co_await resolver->Resolve(udp._Service.value()._Stop);
          if (!peer.has_value()) {
            co_return nullptr;
          }

          channel = std::make_shared<Channel>(udp, id, peer.value());
        } else {
          channel = std::make_shared<Channel>(udp, id);
        }

        auto [it, inserted] = udp._Channels.try_emplace(id, channel);
        assert(inserted);
        auto err = co_await channel->Start();
        if (err) {
          co_return nullptr;
        }
        co_return channel;
      });
  assert(reply.has_value());
  co_return reply.value();
}

Omni::Fiber::Coroutine<void> UdpMux::RemoveChannel(uint8_t id) {
  co_await _ChannelRpc.Call([this, id]() -> Omni::Fiber::Coroutine<void> {
    auto it = _Channels.find(id);
    assert(it != _Channels.end());

    auto channel = std::move(it->second);
    _Channels.erase(it);
    co_await channel->Stop();
    co_await channel->WaitService();
  });
}

Omni::Fiber::Coroutine<void> UdpMux::ReadLoop() {
  while (!_Service.value()._Stop.IsTriggered()) {
    Packet p;
    boost::asio::ip::udp::endpoint peer;

    auto [err, bytes_transferred] =
        co_await _Socket.async_receive_from(boost::asio::mutable_buffer(p), peer, _Service.value()._Stop.AsioSlot()());
    if (err) {
      if (err == boost::asio::error::operation_aborted) {
        BOOST_LOG_TRIVIAL(info) << "UdpMux(" << this << ") read loop cancelled";
      } else {
        BOOST_LOG_TRIVIAL(error) << "UdpMux(" << this << ") read error: " << err.message();
        for (auto& [id, channel] : _Channels) {
          if (channel->GetState() == ServiceBase::State::kRunning) {
            co_await channel->Send(std::unexpected(err));
          }
        }
      }
      break;
    }
    p._Length = bytes_transferred;

    if (p.DataSize() < 1) {
      BOOST_LOG_TRIVIAL(info) << "UdpMux(" << this << ") ignored empty packet from " << peer;
      continue;
    }

    uint8_t id = p.PopFront<uint8_t>();
    auto it = _Channels.find(id);
    if (it != _Channels.end()) {
      if (it->second->GetPeer().has_value()) {
        if (it->second->GetPeer().value() != peer) {
          BOOST_LOG_TRIVIAL(info) << GetName() << " remaps peer " << it->second->GetPeer().value() << " to " << peer
                                  << " for channel " << (int)id;
          it->second->GetPeer() = peer;
        }
      } else {
        BOOST_LOG_TRIVIAL(info) << GetName() << " learns peer " << peer << " for channel " << (int)id;
        it->second->GetPeer() = peer;
      }

      co_await it->second->Send(std::move(p));
    } else {
      BOOST_LOG_TRIVIAL(info) << "UdpMux(" << this << ") packet from unknown channel ID: " << (int)id;
    }
  }
}

Omni::Fiber::Coroutine<ErrorCode> UdpMux::WriteTo(uint8_t id, Packet& p, Cancel& c) {
  if (c.IsTriggered()) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }

  auto it = _Channels.find(id);
  if (it == _Channels.end() || !it->second->GetPeer().has_value()) {
    co_return ErrorCode{AppMinorErrorCategory::kInvalidPacketSession, kAppMinorError};
  }

  if (p.FrontSpace() < 1) {
    co_return ErrorCode{AppErrorCategory::kInvalidPacketReserved, kAppError};
  }

  p.PushFront(id);

  auto [err, bytes_transferred] =
      co_await _Socket.async_send_to(boost::asio::const_buffer(p), it->second->GetPeer().value(), c.AsioSlot()());
  assert(err || bytes_transferred == p._Length);
  co_return err;
}

// ==================== UdpMux::Channel ====================
UdpMux::Channel::Channel(UdpMux& parent, uint8_t id) : _Parent(parent), _Id(id), _Peer(std::nullopt) {}
UdpMux::Channel::Channel(UdpMux& parent, uint8_t id, boost::asio::ip::udp::endpoint peer)
    : _Parent(parent), _Id(id), _Peer(peer) {}

UdpMux::Channel::~Channel() {}

std::string UdpMux::Channel::GetName() const { return std::format("UdpMuxChannel:[{}]", _Id); }

Omni::Fiber::Coroutine<ErrorCode> UdpMux::Channel::DoStart() { co_return ErrorCode{}; }

Omni::Fiber::Coroutine<ErrorCode> UdpMux::Channel::DoGracefulStop() {
  co_await _PipielineUsageCounter.WaitAll();
  co_await _Pipe.GetProducer().Close();
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> UdpMux::Channel::Read(Packet& p, Cancel& c) {
  auto [stopResult, pipeResult] =
      co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(c.GetFiberCancelEvent(), [] {}),
                                   Omni::Fiber::SelectPair(_Pipe.GetConsumer(), [&](auto data) {
                                     if (data.has_value()) {
                                       auto& inner = data.value();
                                       if (inner.has_value()) {
                                         p = std::move(inner.value());
                                         return ErrorCode{};
                                       } else {
                                         return inner.error();
                                       }
                                     } else {
                                       p._Length = 0;
                                       return ErrorCode{AppErrorCategory::kEndOfStream, kAppError};
                                     }
                                   }));
  if (pipeResult.has_value()) {
    co_return pipeResult.value();
  }
  if (stopResult) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }
  assert(false && "should not reach here");
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> UdpMux::Channel::Write(Packet& p, Cancel& c) {
  co_return co_await _Parent.WriteTo(_Id, p, c);
}

} // namespace gh
