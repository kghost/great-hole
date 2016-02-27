#include "endpoint-tun.hpp"

#include <linux/if_tun.h>

tun::tun(boost::asio::io_service &io_service, std::string const &name) :
	boost::asio::posix::basic_descriptor<tun_service>(io_service)
{
	int fd = ::open("/dev/net/tun", O_RDWR);
	if (fd < 0) {
		boost::system::error_code ec;
		ec.assign(errno, boost::system::system_category());
		boost::asio::detail::throw_error(ec, "open");
	}

	try {
		struct ifreq ifr;
		memset(&ifr, 0, sizeof(ifr));
		strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ);
		ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
		if (::ioctl(fd, TUNSETIFF, (void *) &ifr) < 0) {
			boost::system::error_code ec;
			ec.assign(errno, boost::system::system_category());
			boost::asio::detail::throw_error(ec, "tunsetiff");
		}

		boost::system::error_code ec;
		get_service().assign(get_implementation(), fd, ec);
		boost::asio::detail::throw_error(ec, "assign");
	} catch (const boost::system::error_code& ex) {
		::close(fd);
		throw;
	}

	try {
		boost::system::error_code ec;

		get_service().non_blocking(get_implementation(), true, ec);
		boost::asio::detail::throw_error(ec, "non-blocking");
	} catch (const boost::system::error_code& ex) {
		close();
		throw;
	}
}

void tun::async_read(std::function<read_handler> handler) {
	std::shared_ptr<packet> pp(new packet);
	get_service().async_read_some(
		get_implementation(),
		boost::asio::buffer(pp->data.get(), pp->sz),
		[handler, pp](const boost::system::error_code& ec, std::size_t bytes_transferred) {
			pp->sz = bytes_transferred;
			handler(ec, *pp);
		});
}

void tun::async_write(packet &p, std::function<read_handler> handler) {
	std::shared_ptr<packet> pp(new packet(std::move(p)));
	get_service().async_write_some(
		get_implementation(),
		boost::asio::buffer(pp->data.get(), pp->sz),
		[handler, pp](const boost::system::error_code& ec, std::size_t bytes_transferred) {
			pp->sz = bytes_transferred;
			handler(ec, *pp);
		});
}
