#include "EndpointTunSplitIp.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <expected>
#include <fcntl.h>
#include <format>
#include <linux/if_tun.h>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include <boost/asio/buffer.hpp>
#include <boost/log/trivial.hpp>

#include "Cancel.hpp"
#include "ErrorCode.hpp"
#include "GetCurrentOmniFiber.hpp"
#include "Packet.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"
#include "ServiceBase.hpp"
#include "Utils.hpp"

namespace gh {

// ==================== EndpointTunSplitIp ====================
EndpointTunSplitIp::EndpointTunSplitIp(boost::asio::any_io_executor executor, const std::string& name)
    : _TunFileDescriptor(executor), _TunName(name) {}

EndpointTunSplitIp::EndpointTunSplitIp(boost::asio::any_io_executor executor, const std::string& name, int fd)
    : _TunFileDescriptor(executor), _TunName(name) {
  _TunFileDescriptor.assign(fd);
}

EndpointTunSplitIp::~EndpointTunSplitIp() { assert(_Channels.empty()); }

std::string EndpointTunSplitIp::GetName() const { return "TunSplitIp:" + _TunName; }

Omni::Fiber::Coroutine<ErrorCode> EndpointTunSplitIp::DoStart() {
  if (!_TunFileDescriptor.is_open()) {
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
  }

  _TunFileDescriptor.non_blocking(true);
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<void> EndpointTunSplitIp::DoWork() {
  _ReadLoopFiber =
      (co_await Omni::Fiber::GetCurrentOmniFiber())
          .Spawn("EndpointTunSplitIp ReadLoop:" + _TunName + "@" + std::to_string(reinterpret_cast<uintptr_t>(this)),
                 [this]() -> Omni::Fiber::Coroutine<void> {
                   co_await ReadLoop();
                   co_return;
                 });

  bool stopped = false;
  while (!stopped) {
    auto [stopResult, rpcResult] = co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(), [] {}),
        Omni::Fiber::SelectPair(_ChannelRpc.GetServiceAwaitor(), _ChannelRpc.HandleRequest));
    if (stopResult) {
      stopped = true;
    }
    if (rpcResult.has_value() && !rpcResult.value()) {
      stopped = true;
    }
  }
}

Omni::Fiber::Coroutine<ErrorCode> EndpointTunSplitIp::DoGracefulStop() {
  _ChannelRpc.DiscardAndClose();
  for (auto& channel : std::exchange(_Channels, {}) | std::ranges::views::values | std::ranges::to<std::set>()) {
    co_await channel->Stop();
    co_await channel->WaitService();
  }
  if (_ReadLoopFiber) {
    co_await (co_await Omni::Fiber::GetCurrentOmniFiber()).Join(_ReadLoopFiber);
    _ReadLoopFiber.reset();
  }
  _TunFileDescriptor.close();
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<std::shared_ptr<EndpointTunSplitIp::Channel>>
EndpointTunSplitIp::CreateChannel(const std::vector<boost::asio::ip::address_v6>& ips) {
  assert(!ips.empty());
  if (ips.empty()) {
    co_return nullptr;
  }

  auto sortedIps = ips;
  std::sort(sortedIps.begin(), sortedIps.end());

  auto reply = co_await _ChannelRpc.Call(
      [&tun = *this, sortedIps](this auto self) -> Omni::Fiber::Coroutine<std::shared_ptr<Channel>> {
        for (auto const& ip : sortedIps) {
          if (tun._Channels.contains(ip)) {
            co_return nullptr;
          }
        }
        auto channel = std::make_shared<Channel>(tun, sortedIps);
        auto err = co_await channel->Start();
        if (err) {
          co_return nullptr;
        }
        for (auto const& ip : sortedIps) {
          tun._Channels.emplace(ip, channel);
        }
        co_return channel;
      });
  assert(reply.has_value());
  co_return reply.value();
}

Omni::Fiber::Coroutine<void> EndpointTunSplitIp::RemoveChannel(std::shared_ptr<EndpointTunSplitIp::Channel> channel) {
  if (!channel) {
    co_return;
  }
  co_await _ChannelRpc.Call([this, channel]() -> Omni::Fiber::Coroutine<void> {
    for (auto const& chIp : channel->GetIps()) {
      _Channels.erase(chIp);
    }
    co_await channel->Stop();
    co_await channel->WaitService();
  });
}

Omni::Fiber::Coroutine<void> EndpointTunSplitIp::ReadLoop() {
  while (!_Service.value()._Stop.IsTriggered()) {
    Packet p;

    auto [err, bytesTransferred] = co_await _TunFileDescriptor.async_read_some(boost::asio::mutable_buffer(p),
                                                                               _Service.value()._Stop.AsioSlot()());
    if (err) {
      if (err == boost::asio::error::operation_aborted) {
        BOOST_LOG_TRIVIAL(info) << "EndpointTunSplitIp(" << this << ") read loop cancelled";
      } else {
        BOOST_LOG_TRIVIAL(error) << "EndpointTunSplitIp(" << this << ") read error: " << err.message();
        for (auto& [ip, channel] : _Channels) {
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
    p._Length = bytesTransferred;

    auto destIpOpt = GetDestAddress(p);
    if (!destIpOpt.has_value()) {
      BOOST_LOG_TRIVIAL(debug) << "EndpointTunSplitIp(" << this << ") dropped invalid packet size/format";
      continue;
    }

    auto it = _Channels.find(*destIpOpt);
    if (it != _Channels.end()) {
      auto result = co_await it->second->Send(std::move(p));
      if (!result.has_value()) {
        BOOST_LOG_TRIVIAL(error) << it->second->GetName() << " send failed.";
      }
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

  auto [err, bytesTransferred] =
      co_await _TunFileDescriptor.async_write_some(boost::asio::const_buffer(p), c.AsioSlot()());
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
  _Pipe.GetConsumer().DiscardAndClose();
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> EndpointTunSplitIp::Channel::Read(Packet& p, Cancel& c) {
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

Omni::Fiber::Coroutine<ErrorCode> EndpointTunSplitIp::Channel::Write(Packet& p, Cancel& c) {
  auto srcIpOpt = GetSourceAddress(p);
  if (!std::binary_search(_Ips.begin(), _Ips.end(), srcIpOpt)) {
    BOOST_LOG_TRIVIAL(warning) << "Channel " << GetName() << " drop packet: source IP "
                               << (srcIpOpt.has_value() ? srcIpOpt->to_string() : "invalid/unknown")
                               << " does not match channel IPs";
    co_return ErrorCode(AppMinorErrorCategory::kSourceIpMismatch, kAppMinorError);
  }
  co_return co_await _Parent.WriteToTun(p, c);
}

} // namespace gh
