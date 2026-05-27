#include "endpoint-tun.hpp"

#include <linux/if_tun.h>
#include <memory>

#include <boost/log/trivial.hpp>

#include "Asio.hpp"
#include "utils.hpp"

namespace gh {

Tun::Tun(boost::asio::io_context& io_context, std::string const& name) : _S(io_context), _Name(name) {}

Omni::Fiber::Coroutine<ErrorCode> Tun::Start(Omni::Fiber::Event<>& stopSignal) {
  co_return co_await BackgroundStart(
      _IsStarted, _StartedError, stopSignal, [this]() -> Omni::Fiber::Coroutine<ErrorCode> { return DoStart(); },
      [this]() -> Omni::Fiber::Coroutine<void> {
        _S.close();
        co_return;
      });
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

Omni::Fiber::Coroutine<std::tuple<ErrorCode, Packet>> Tun::Read() {
  auto a = std::make_shared<std::array<uint8_t, 2048>>();
  auto p = Packet{Buffer(*a), a};
  auto buffer = boost::asio::mutable_buffer{p.first};
  auto [err, bytes_transferred] = co_await _S.async_read_some(buffer, Omni::Fiber::AsioUseFiber);
  if (!err) {
    assert(bytes_transferred <= p.first.Capacity - p.first.Offset);
    p.first.Length = bytes_transferred;
  }
  co_return std::make_tuple(err, std::move(p));
}

Omni::Fiber::Coroutine<std::tuple<ErrorCode, std::size_t>> Tun::Write(Packet&& p) {
  co_return co_await _S.async_write_some(boost::asio::const_buffer{p.first}, Omni::Fiber::AsioUseFiber);
}

} // namespace gh
