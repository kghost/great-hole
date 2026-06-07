#include "EndpointTunSplitIp.hpp"

#include <fcntl.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <boost/asio/buffer.hpp>
#include <boost/log/trivial.hpp>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "ErrorCode.hpp"
#include "GetCurrentFiber.hpp"
#include "Packet.hpp"
#include "Select.hpp"
#include "ServiceBase.hpp"
#include "Utils.hpp"

namespace gh {

// ==================== EndpointTunSplitIp ====================
EndpointTunSplitIp::EndpointTunSplitIp(boost::asio::io_context& ioContext, const std::string& name)
    : _TunFileDescriptor(ioContext), _TunName(name) {}

EndpointTunSplitIp::EndpointTunSplitIp(boost::asio::io_context& ioContext, int testFd)
    : _TunFileDescriptor(ioContext), _TunName("test"), _TestFd(testFd) {}

EndpointTunSplitIp::~EndpointTunSplitIp() { assert(_Channels.empty()); }

std::string EndpointTunSplitIp::GetName() const { return "TunSplitIp:" + _TunName; }

Omni::Fiber::Coroutine<ErrorCode> EndpointTunSplitIp::DoStart() {
  if (_TestFd != -1) {
    _TunFileDescriptor.assign(_TestFd);
    _TunFileDescriptor.non_blocking(true);
    co_return ErrorCode{};
  }

  int fd = ::open("/dev/net/tun", O_RDWR);
  if (fd < 0) {
    co_return ErrorCode(errno, system_category());
  }

  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, _TunName.c_str(), IFNAMSIZ);
  ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
  if (::ioctl(fd, TUNSETIFF, (void*)&ifr) < 0) {
    ::close(fd);
    co_return ErrorCode(errno, system_category());
  }

  _TunFileDescriptor.assign(fd);
  _TunFileDescriptor.non_blocking(true);
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<void> EndpointTunSplitIp::DoWork() {
  (co_await Omni::Fiber::GetCurrentFiber())
      .Spawn("EndpointTunSplitIp ReadLoop:" + _TunName + "@" + std::to_string(reinterpret_cast<uintptr_t>(this)),
             [this]() -> Omni::Fiber::Coroutine<void> {
               co_await ReadLoop();
               co_return;
             });

  bool stopped = false;
  while (!stopped) {
    co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(), [&]() { stopped = true; }),
        Omni::Fiber::SelectPair(_ChannelRpc.GetServiceAwaitor(), [&](auto req) -> Omni::Fiber::Coroutine<void> {
          if (!co_await Omni::Fiber::RemoteCall::HandleRequest(std::move(req))) {
            stopped = true;
          }
          co_return;
        }));
  }
}

Omni::Fiber::Coroutine<ErrorCode> EndpointTunSplitIp::DoGracefulStop() {
  co_await _ChannelRpc.Close();
  for (auto& [ip, channel] : std::exchange(_Channels, {})) {
    co_await channel->Stop();
    co_await channel->WaitService();
  }
  co_await (co_await Omni::Fiber::GetCurrentFiber()).WaitAll();
  _TunFileDescriptor.close();
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<std::shared_ptr<EndpointTunSplitIp::Channel>>
EndpointTunSplitIp::CreateChannel(const std::vector<boost::asio::ip::address_v6>& ips) {
  assert(!ips.empty());
  if (ips.empty()) {
    co_return nullptr;
  }

  std::vector<boost::asio::ip::address_v6> v6Ips;
  for (auto const& ip : ips) {
    v6Ips.push_back(MapToV6(ip));
  }
  std::sort(v6Ips.begin(), v6Ips.end());
  v6Ips.erase(std::unique(v6Ips.begin(), v6Ips.end()), v6Ips.end());

  auto reply = co_await _ChannelRpc.Call(
      [&tun = *this, v6Ips](this auto self) -> Omni::Fiber::Coroutine<std::shared_ptr<Channel>> {
        for (auto const& ip : v6Ips) {
          if (tun._Channels.contains(ip)) {
            co_return nullptr;
          }
        }
        auto channel = std::make_shared<Channel>(tun, v6Ips);
        auto err = co_await channel->Start();
        if (err) {
          co_return nullptr;
        }
        for (auto const& ip : v6Ips) {
          tun._Channels.emplace(ip, channel);
        }
        co_return channel;
      });
  assert(reply.has_value());
  co_return reply.value();
}

Omni::Fiber::Coroutine<void> EndpointTunSplitIp::RemoveChannel(const boost::asio::ip::address_v6& ip) {
  co_await _ChannelRpc.Call([this, ip]() -> Omni::Fiber::Coroutine<void> {
    auto it = _Channels.find(ip);
    assert(it != _Channels.end());

    auto channel = std::move(it->second);
    for (auto const& chIp : channel->GetIps()) {
      _Channels.erase(chIp);
    }
    co_await channel->Stop();
    co_await channel->WaitService();
  });
}

Omni::Fiber::Coroutine<void> EndpointTunSplitIp::ReadLoop() {
  auto slotTracker = _Service.value()._Stop.AsioSlot();
  while (!_Service.value()._Stop.IsTriggered()) {
    Packet p;

    auto [err, bytesTransferred] = co_await _TunFileDescriptor.async_read_some(
        boost::asio::mutable_buffer(p),
        boost::asio::bind_cancellation_slot(slotTracker.Slot(), Omni::Fiber::AsioUseFiber));
    if (err) {
      if (err == boost::asio::error::operation_aborted) {
        BOOST_LOG_TRIVIAL(info) << "EndpointTunSplitIp(" << this << ") read loop cancelled";
      } else {
        BOOST_LOG_TRIVIAL(error) << "EndpointTunSplitIp(" << this << ") read error: " << err.message();
        for (auto& [ip, channel] : _Channels) {
          if (channel->GetState() == ServiceBase::State::kRunning) {
            co_await channel->Send(std::unexpected(err));
          }
        }
      }
      break;
    }
    p._Length = bytesTransferred;

    auto destIpOpt = GetDestAddress(p);
    if (!destIpOpt.has_value()) {
      BOOST_LOG_TRIVIAL(debug) << "EndpointTunSplitIp(" << this << ") dropped invalid packet size/format";
      continue;
    }

    auto it = _Channels.find(*destIpOpt);
    if (it != _Channels.end()) {
      co_await it->second->Send(std::move(p));
    } else {
      BOOST_LOG_TRIVIAL(debug) << "EndpointTunSplitIp(" << this
                               << ") dropped packet to unknown dest IP: " << destIpOpt->to_string();
    }
  }
}

Omni::Fiber::Coroutine<ErrorCode> EndpointTunSplitIp::WriteToTun(Packet& p, Cancel& c) {
  if (c.IsTriggered()) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }

  auto [err, bytesTransferred] = co_await _TunFileDescriptor.async_write_some(
      boost::asio::const_buffer(p),
      boost::asio::bind_cancellation_slot(c.AsioSlot().Slot(), Omni::Fiber::AsioUseFiber));
  assert(err || bytesTransferred == p._Length);
  co_return err;
}

std::optional<boost::asio::ip::address_v6> EndpointTunSplitIp::GetSourceAddress(const Packet& p) {
  if (p.DataSize() < 20) {
    return std::nullopt;
  }
  uint8_t version = (p.Data()[0] >> 4);
  if (version == 4) {
    std::array<uint8_t, 4> addrBytes;
    std::copy_n(p.Data().data() + 12, 4, addrBytes.begin());
    return MapToV6(boost::asio::ip::make_address_v4(addrBytes));
  } else if (version == 6) {
    if (p.DataSize() < 40) {
      return std::nullopt;
    }
    std::array<uint8_t, 16> addrBytes;
    std::copy_n(p.Data().data() + 8, 16, addrBytes.begin());
    return boost::asio::ip::make_address_v6(addrBytes);
  }
  return std::nullopt;
}

std::optional<boost::asio::ip::address_v6> EndpointTunSplitIp::GetDestAddress(const Packet& p) {
  if (p.DataSize() < 20) {
    return std::nullopt;
  }
  uint8_t version = (p.Data()[0] >> 4);
  if (version == 4) {
    std::array<uint8_t, 4> addrBytes;
    std::copy_n(p.Data().data() + 16, 4, addrBytes.begin());
    return MapToV6(boost::asio::ip::make_address_v4(addrBytes));
  } else if (version == 6) {
    if (p.DataSize() < 40) {
      return std::nullopt;
    }
    std::array<uint8_t, 16> addrBytes;
    std::copy_n(p.Data().data() + 24, 16, addrBytes.begin());
    return boost::asio::ip::make_address_v6(addrBytes);
  }
  return std::nullopt;
}

// ==================== EndpointTunSplitIp::Channel ====================
EndpointTunSplitIp::Channel::Channel(EndpointTunSplitIp& parent, const std::vector<boost::asio::ip::address_v6>& ips)
    : _Parent(parent), _Ips(ips) {}

EndpointTunSplitIp::Channel::~Channel() {}

std::string EndpointTunSplitIp::Channel::GetName() const {
  return std::format("TunSplitIpChannel:[{}]", _Ips.front().to_string());
}

Omni::Fiber::Coroutine<ErrorCode> EndpointTunSplitIp::Channel::DoStart() { co_return ErrorCode{}; }

Omni::Fiber::Coroutine<ErrorCode> EndpointTunSplitIp::Channel::DoGracefulStop() {
  co_await _PipielineUsageCounter.WaitAll();
  co_await _Pipe.GetProducer().Close();
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> EndpointTunSplitIp::Channel::Read(Packet& p, Cancel& c) {
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

Omni::Fiber::Coroutine<ErrorCode> EndpointTunSplitIp::Channel::Write(Packet& p, Cancel& c) {
  auto srcIpOpt = GetSourceAddress(p);
  if (!std::binary_search(_Ips.begin(), _Ips.end(), srcIpOpt)) {
    BOOST_LOG_TRIVIAL(warning) << "Channel drop packet: source IP "
                               << (srcIpOpt.has_value() ? srcIpOpt->to_string() : "invalid/unknown")
                               << " does not match channel IPs";
    co_return ErrorCode(AppMinorErrorCategory::kSourceIpMismatch, kAppMinorError);
  }
  co_return co_await _Parent.WriteToTun(p, c);
}

} // namespace gh
