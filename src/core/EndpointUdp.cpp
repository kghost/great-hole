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

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "GetCurrentOmniFiber.hpp"
#include "RemoteCall.hpp"
#include "ResolverStaticEndpoint.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"
#include "ServiceBase.hpp"

namespace gh {

// ==================== Udp ====================
Udp::Udp(boost::asio::any_io_executor executor) : _Socket(executor), _Local(boost::asio::ip::udp::v6(), 0) {}
Udp::Udp(boost::asio::any_io_executor executor, boost::asio::ip::udp::endpoint bind)
    : _Socket(executor), _Local(bind) {}
Udp::~Udp() { assert(_Channels.empty()); }

auto Udp::GetName() const -> std::string { return "Udp:" + boost::lexical_cast<std::string>(_Local); }

auto Udp::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> {
  ErrorCode ec;
  try {
    _Socket.open(boost::asio::ip::udp::v6());
    _Socket.set_option(boost::asio::ip::v6_only(false));
    _Socket.bind(_Local);
    BOOST_LOG_TRIVIAL(info) << "udp(" << this << ") bound at " << _Socket.local_endpoint();
  } catch (const SystemError& e) {
    BOOST_LOG_TRIVIAL(info) << "udp(" << this << ") start failed: " << e.what();
    ec = e.code();
  }

  if (ec) {
    _ChannelRpc.DiscardAndClose();
    co_return ec;
  }
  co_return ErrorCode{};
}

auto Udp::DoWork() -> Omni::Fiber::Coroutine<void> {
  _ReadLoopFiber = (co_await Omni::Fiber::GetCurrentOmniFiber())
                       .Spawn("Udp ReadLoop:" + boost::lexical_cast<std::string>(LocalEndpoint()) + "@" +
                                  std::to_string(reinterpret_cast<uintptr_t>(this)),
                              [this]() -> Omni::Fiber::Coroutine<void> {
                                co_await ReadLoop();
                                co_return;
                              });

  bool stopped = false;
  while (!stopped) {
    auto [stopResult, rpcResult] = co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(), [] -> void {}),
        Omni::Fiber::SelectPair(_ChannelRpc.GetServiceAwaitor(), Omni::Fiber::RemoteCall::HandleRequest));
    if (stopResult) {
      stopped = true;
    }
    if (rpcResult.has_value() && !rpcResult.value()) {
      stopped = true;
    }
  }
}

auto Udp::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> {
  _ChannelRpc.DiscardAndClose();
  for (auto& [peer, channel] : std::exchange(_Channels, {})) {
    co_await channel->Stop();
  }
  if (_ReadLoopFiber) {
    co_await (co_await Omni::Fiber::GetCurrentOmniFiber()).Join(_ReadLoopFiber);
    _ReadLoopFiber.reset();
  }
  _Socket.close();
  co_return ErrorCode{};
}

auto Udp::CreateChannel(std::shared_ptr<ResolverEndpoint> resolver)
    -> Omni::Fiber::Coroutine<std::shared_ptr<Udp::UdpChannel>> {
  auto result = co_await _ChannelRpc.Call(
      [&udp = *this, resolver](this auto self) -> Omni::Fiber::Coroutine<std::shared_ptr<Udp::UdpChannel>> {
        auto peer = co_await resolver->Resolve(udp._Service.value()._Stop);
        if (!peer.has_value()) {
          co_return nullptr;
        }

        auto ep = peer.value();
        auto channel = std::make_shared<UdpChannel>(udp, ep);
        auto [it, inserted] = udp._Channels.try_emplace(ep, channel);
        assert(inserted);
        auto err = co_await channel->Start();
        if (err) {
          co_return nullptr;
        }
        co_return channel;
      });
  co_return result.value();
}

auto Udp::CreateChannel(boost::asio::ip::udp::endpoint const& peer)
    -> Omni::Fiber::Coroutine<std::shared_ptr<Udp::UdpChannel>> {
  co_return co_await CreateChannel(std::make_shared<ResolverStaticEndpoint>(peer));
}

auto Udp::RemoveChannel(const boost::asio::ip::udp::endpoint& peer) -> Omni::Fiber::Coroutine<void> {
  auto result = co_await _ChannelRpc.Call([this, peer]() -> Omni::Fiber::Coroutine<void> {
    auto it = _Channels.find(peer);
    assert(it != _Channels.end());

    auto channel = std::move(it->second);
    _Channels.erase(it);
    co_await channel->Stop();
  });
  if (!result.has_value()) {
    BOOST_LOG_TRIVIAL(error) << GetName() << " remove channel failed";
  }
}

auto Udp::ReadLoop() -> Omni::Fiber::Coroutine<void> {
  while (!_Service.value()._Stop.IsTriggered()) {
    Packet p;
    boost::asio::ip::udp::endpoint peer;

    auto [err, bytes_transferred] =
        co_await _Socket.async_receive_from(boost::asio::mutable_buffer(p), peer, _Service.value()._Stop.AsioSlot()());
    if (err) {
      if (err == boost::asio::error::operation_aborted) {
        BOOST_LOG_TRIVIAL(info) << "udp(" << this << ") read loop cancelled";
      } else {
        BOOST_LOG_TRIVIAL(error) << "udp(" << this << ") read error: " << err.message();
        for (auto& [endpoint, channel] : _Channels) {
          if (channel->GetState() == ServiceBase::State::kRunning) {
            auto result = co_await channel->Send(std::unexpected(err));
            if (!result.has_value()) {
              BOOST_LOG_TRIVIAL(error) << channel->GetName() << " send failed.";
            }
          }
        }
      }
      break;
    }

    p._Length = bytes_transferred;
    auto it = _Channels.find(peer);
    if (it != _Channels.end()) {
      auto result = co_await it->second->Send(std::move(p));
      if (!result.has_value()) {
        BOOST_LOG_TRIVIAL(error) << it->second->GetName() << " send failed.";
      }
    } else {
      BOOST_LOG_TRIVIAL(info) << "udp(" << this << ") packet from unknown peer: " << peer;
    }
  }
}

auto Udp::WriteTo(boost::asio::ip::udp::endpoint const& peer, Packet& p, Cancel& c)
    -> Omni::Fiber::Coroutine<ErrorCode> {
  if (c.IsTriggered()) {
    co_return Error(AppErrorCategory::kOperationAborted);
  }
  auto [err, bytes_transferred] = co_await _Socket.async_send_to(boost::asio::const_buffer(p), peer, c.AsioSlot()());
  assert(err || bytes_transferred == p._Length);
  co_return err;
}

// ==================== UdpChannel ====================
Udp::UdpChannel::UdpChannel(Udp& parent, boost::asio::ip::udp::endpoint peer) : _Parent(parent), _Peer(peer) {}
Udp::UdpChannel::~UdpChannel() {}

auto Udp::UdpChannel::GetName() const -> std::string {
  return std::format("UdpChannel({:p})@({}):{}", static_cast<const void*>(this),
                     boost::lexical_cast<std::string>(_Parent.LocalEndpoint()),
                     boost::lexical_cast<std::string>(_Peer));
}

auto Udp::UdpChannel::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> { co_return ErrorCode{}; }

auto Udp::UdpChannel::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> {
  co_await _PipielineUsageCounter.WaitAll();
  _Pipe.GetConsumer().DiscardAndClose();
  co_return ErrorCode{};
}

auto Udp::UdpChannel::Read(Packet& p, Cancel& c) -> Omni::Fiber::Coroutine<ErrorCode> {
  auto [stopResult, pipeResult] =
      co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(c.GetFiberCancelEvent(), [] -> void {}),
                                   Omni::Fiber::SelectPair(_Pipe.GetConsumer(), [&](auto data) -> auto {
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
                                       return Error(AppErrorCategory::kEndOfStream);
                                     }
                                   }));
  if (pipeResult.has_value()) {
    co_return pipeResult.value();
  }
  if (stopResult) {
    co_return Error(AppErrorCategory::kOperationAborted);
  }
  assert(false && "should not reach here");
  co_return ErrorCode{};
}

auto Udp::UdpChannel::Write(Packet& p, Cancel& c) -> Omni::Fiber::Coroutine<ErrorCode> {
  co_return co_await _Parent.WriteTo(_Peer, p, c);
}

} // namespace gh
