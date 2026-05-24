#include "endpoint-tun.hpp"

#include <linux/if_tun.h>
#include <memory>

#include <boost/log/trivial.hpp>

#include "util-exec.hpp"

namespace gh {

Tun::Tun(boost::asio::io_context& io_context, std::string const& name) : _S(io_context), _Name(name) {}
Tun::Tun(boost::asio::io_context& io_context, std::string const& name, std::shared_ptr<Exec> e)
    : _S(io_context), _Name(name), _E(e) {}

void Tun::AsyncStart(std::move_only_function<Event>&& handler) {
  if (_Started == true) {
    handler(_StartedEc);
    return;
  }

  _Started = true;
  int fd = ::open("/dev/net/tun", O_RDWR);
  if (fd < 0) {
    _StartedEc = ErrorCode(errno, system_category());
    handler(_StartedEc);
    return;
  }

  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, _Name.c_str(), IFNAMSIZ);
  ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
  if (::ioctl(fd, TUNSETIFF, (void*)&ifr) < 0) {
    ::close(fd);
    _StartedEc = ErrorCode(errno, system_category());
    handler(_StartedEc);
    return;
  }

  _StartedEc = _S.assign(fd, _StartedEc);
  if (_StartedEc) {
    ::close(fd);
    handler(_StartedEc);
    return;
  }
  _StartedEc = _S.non_blocking(true, _StartedEc);
  if (_StartedEc) {
    _S.close();
    handler(_StartedEc);
    return;
  }
  if (_E) {
    _E->Run([me = shared_from_this(), handler{std::move(handler)}](const ErrorCode& ec) mutable { handler(ec); });
    _E.reset();
  } else {
    handler(_StartedEc);
  }
}

void Tun::AsyncRead(std::move_only_function<ReadHandler>&& handler) {
  auto a = std::make_shared<std::array<uint8_t, 2048>>();
  auto p = Packet{Buffer(*a), a};
  auto buffer = boost::asio::mutable_buffer{p.first};
  _S.async_read_some(buffer, [me = shared_from_this(), p{std::move(p)},
                              handler{std::move(handler)}](const ErrorCode& ec, std::size_t bytes_transferred) mutable {
    if (!ec) {
      assert(bytes_transferred <= p.first.Capacity - p.first.Offset);
      p.first.Length = bytes_transferred;
    }
    handler(ec, std::move(p));
  });
}

void Tun::AsyncWrite(Packet&& p, std::move_only_function<WriteHandler>&& handler) {
  _S.async_write_some(boost::asio::const_buffer{p.first}, std::move(handler));
}

} // namespace gh
