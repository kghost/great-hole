#include "config.h"

#include "endpoint-tun.hpp"

#include <linux/if_tun.h>
#include <memory>

#include "util-exec.hpp"

tun::tun(boost::asio::io_service &io_service, std::string const &name) :
	boost::asio::posix::basic_descriptor<tun_service>(io_service), name(name) {}

tun::tun(boost::asio::io_service &io_service, std::string const &name, std::shared_ptr<exec> e) :
	boost::asio::posix::basic_descriptor<tun_service>(io_service), name(name), e(e) {}

void tun::async_start(std::function<event> &&handler) {
	int fd = ::open("/dev/net/tun", O_RDWR);
	if (fd < 0) { handler(gh::error_code(errno, gh::system_category())); return; }

	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ);
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
	if (::ioctl(fd, TUNSETIFF, (void *) &ifr) < 0) {
		::close(fd);
		handler(gh::error_code(errno, gh::system_category()));
		return;
	}

	gh::error_code ec;
	get_service().assign(get_implementation(), fd, ec);
	if (ec) { ::close(fd); handler(ec); return; }
	get_service().non_blocking(get_implementation(), true, ec);
	if (ec) { close(); handler(ec); return; }
	if (e) {
		e->run([me = shared_from_this(), handler, e = e] (gh::error_code ec) { handler(ec); });
		e.reset();
	} else {
		handler(ec);
	}
}

void tun::async_read(std::function<read_handler> &&handler) {
	auto a = std::make_shared<std::array<char, 2048>>();
	auto p = packet{{a.get(), a->size()}, a};
	get_service().async_read_some(
		get_implementation(), p.first,
		[p, handler = std::move(handler)](const gh::error_code& ec, std::size_t bytes_transferred) mutable {
			handler(ec, packet{boost::asio::buffer(p.first, bytes_transferred), p.second});
		});
}

void tun::async_write(boost::asio::const_buffers_1 const &b, std::function<write_handler> &&handler) {
	get_service().async_write_some(get_implementation(), b, handler);
}
