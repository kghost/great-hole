#include "endpoint-tun.hpp"

#include <boost/asio/buffer.hpp>
#include <linux/if_tun.h>

#include <boost/log/trivial.hpp>

#include "Asio.hpp"
#include "utils.hpp"

namespace gh {

Tun::Tun(boost::asio::io_context& io_context, std::string const& name) : _S(io_context), _Name(name) {}

Omni::Fiber::Coroutine<ErrorCode> Tun::Start(Omni::Fiber::Event<>& stopSignal) {
  co_return co_await BackgroundStart(
      "Start tun:" + _Name, _IsStarted, _StartedError, stopSignal,
      [this]() -> Omni::Fiber::Coroutine<ErrorCode> { return DoStart(); },
      [this]() -> Omni::Fiber::Coroutine<void> { co_return _S.close(); });
}

Omni::Fiber::Coroutine<ErrorCode> Tun::DoStart() {
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

  _S.assign(fd);
  _S.non_blocking(true);
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> Tun::Read(Packet& p) {
  auto [err, bytes_transferred] =
      co_await _S.async_read_some(boost::asio::mutable_buffer(p), Omni::Fiber::AsioUseFiber);
  p._Length = bytes_transferred;
  co_return err;
}

Omni::Fiber::Coroutine<ErrorCode> Tun::Write(Packet& p) {
  auto [err, bytes_transferred] = co_await _S.async_write_some(boost::asio::const_buffer(p), Omni::Fiber::AsioUseFiber);
  assert(p._Length == bytes_transferred);
  co_return err;
}

} // namespace gh
