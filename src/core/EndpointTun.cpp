#include "EndpointTun.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/log/trivial.hpp>
#include <format>
#include <linux/if_tun.h>

#include "ErrorCode.hpp"

namespace gh {

Tun::Tun(boost::asio::any_io_executor executor, std::string const& name) : _TunFileDescriptor(executor), _Name(name) {}

Tun::Tun(boost::asio::any_io_executor executor, std::string const& name, int fd)
    : _TunFileDescriptor(executor), _Name(name) {
  _TunFileDescriptor.assign(fd);
  _TunFileDescriptor.non_blocking(true);
}

auto Tun::GetName() const -> std::string {
  return std::format("Tun:{}[{}]", _Name,
                     const_cast<boost::asio::posix::stream_descriptor&>(_TunFileDescriptor).native_handle());
}

auto Tun::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> {
  if (_TunFileDescriptor.is_open()) {
    co_return ErrorCode{};
  }
  int fd = ::open("/dev/net/tun", O_RDWR);
  if (fd < 0) {
    co_return ErrorCode(errno, system_category());
  }

  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, _Name.c_str(), IFNAMSIZ);
  ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
  if (::ioctl(fd, TUNSETIFF, (void*)&ifr) < 0) {
    ::close(fd);
    co_return ErrorCode(errno, system_category());
  }

  _TunFileDescriptor.assign(fd);
  _TunFileDescriptor.non_blocking(true);
  co_return ErrorCode{};
}

auto Tun::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> {
  co_await _PipielineUsageCounter.WaitAll();
  _TunFileDescriptor.close();
  co_return ErrorCode{};
}

auto Tun::Read(Packet& p, Cancel& c) -> Omni::Fiber::Coroutine<ErrorCode> {
  if (c.IsTriggered()) {
    co_return Error(AppErrorCategory::kOperationAborted);
  }
  auto [err, bytes_transferred] =
      co_await _TunFileDescriptor.async_read_some(boost::asio::mutable_buffer(p), c.AsioSlot()());
  p._Length = bytes_transferred;
  co_return err;
}

auto Tun::Write(Packet& p, Cancel& c) -> Omni::Fiber::Coroutine<ErrorCode> {
  if (c.IsTriggered()) {
    co_return Error(AppErrorCategory::kOperationAborted);
  }
  auto [err, bytes_transferred] =
      co_await _TunFileDescriptor.async_write_some(boost::asio::const_buffer(p), c.AsioSlot()());
  assert(err || p._Length == bytes_transferred);
  co_return err;
}

} // namespace gh
