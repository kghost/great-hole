#include "endpoint-tun.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/log/trivial.hpp>
#include <linux/if_tun.h>

#include "Asio.hpp"
#include "GetCurrentFiber.hpp"

namespace gh {

Tun::Tun(boost::asio::io_context& io_context, std::string const& name) : _TunFileDescriptor(io_context), _Name(name) {}

Omni::Fiber::Coroutine<ErrorCode> Tun::Start() {
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

  auto& fiber = co_await Omni::Fiber::GetCurrentFiber();
  fiber.Spawn("Start Tun:" + _Name, [this]() -> Omni::Fiber::Coroutine<void> {
    auto me = std::dynamic_pointer_cast<Tun>(shared_from_this());
    co_await _Stop.GetFiberCancelEvent();
    if (_PipelineCount > 0) {
      co_await _GracefulExitEvent;
    }
    assert(_PipelineCount = 0);
    BOOST_LOG_TRIVIAL(info) << "Tun(" << this << ") graceful exit triggered, closing descriptor";
    _TunFileDescriptor.close();
    co_return;
  });

  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> Tun::Read(Packet& p, Cancel& c) {
  auto [err, bytes_transferred] =
      co_await _TunFileDescriptor.async_read_some(boost::asio::mutable_buffer(p), Omni::Fiber::AsioUseFiber);
  p._Length = bytes_transferred;
  co_return err;
}

Omni::Fiber::Coroutine<ErrorCode> Tun::Write(Packet& p, Cancel& c) {
  auto [err, bytes_transferred] =
      co_await _TunFileDescriptor.async_write_some(boost::asio::const_buffer(p), Omni::Fiber::AsioUseFiber);
  assert(p._Length == bytes_transferred);
  co_return err;
}

Omni::Fiber::Coroutine<ErrorCode> Tun::Stop() {
  _Stop.Trigger();
  co_return ErrorCode{};
}

} // namespace gh
