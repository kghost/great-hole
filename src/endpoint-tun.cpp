#include "config.h"

#include "endpoint-tun.hpp"

#include <linux/if_tun.h>
#include <memory>

#include <boost/log/trivial.hpp>

#include "util-exec.hpp"

tun::tun(boost::asio::io_service &io_service, std::string const &name) :
	boost::asio::posix::basic_descriptor<tun_service>(io_service), name(name) {}

tun::tun(boost::asio::io_service &io_service, std::string const &name, std::shared_ptr<exec> e) :
	boost::asio::posix::basic_descriptor<tun_service>(io_service), name(name), e(e) {}

void tun::async_start(fu2::unique_function<event> &&handler) {
	if (started == true) {
		handler(started_ec);
		return;
	}

	started = true;
	int fd = ::open("/dev/net/tun", O_RDWR);
	if (fd < 0) {
		started_ec = gh::error_code(errno, gh::system_category());
		handler(started_ec);
		return;
	}

	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ);
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
	if (::ioctl(fd, TUNSETIFF, (void *) &ifr) < 0) {
		::close(fd);
		started_ec = gh::error_code(errno, gh::system_category());
		handler(started_ec);
		return;
	}

	get_service().assign(get_implementation(), fd, started_ec);
	if (started_ec) { ::close(fd); handler(started_ec); return; }
	get_service().non_blocking(get_implementation(), true, started_ec);
	if (started_ec) { close(); handler(started_ec); return; }
	if (e) {
		e->run([me = shared_from_this(), handler{std::move(handler)}, e = e] (boost::system::error_code ec) mutable { handler(ec); });
		e.reset();
	} else {
		handler(started_ec);
	}
}

void tun::async_read(fu2::unique_function<read_handler> &&handler) {
	auto a = std::make_shared<std::array<uint8_t, 2048>>();
	auto p = packet{buffer(*a), a};
	auto buffer = boost::asio::mutable_buffer{p.first};
	get_service().async_read_some(
		get_implementation(), buffer,
		[me = shared_from_this(), p{std::move(p)}, handler{std::move(handler)}](const gh::error_code& ec, std::size_t bytes_transferred) mutable {
			if (!ec) {
				assert(bytes_transferred <= p.first.capacity - p.first.offset);
				p.first.length = bytes_transferred;
			}
			handler(ec, std::move(p));
		});
}

void tun::async_write(packet && p, fu2::unique_function<write_handler> &&handler) {
	get_service().async_write_some(get_implementation(), boost::asio::const_buffer{p.first}, std::move(handler));
}
