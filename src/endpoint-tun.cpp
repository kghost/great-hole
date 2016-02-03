#include "endpoint-tun.hpp"

#include <linux/if_tun.h>

tun::tun(boost::asio::io_service &io_service) :
	boost::asio::posix::basic_descriptor<tun_service>(io_service)
{
	int fd = ::open("/dev/net/tun", O_RDWR);
	if (fd < 0) {
		boost::system::error_code ec;
		ec.assign(fd, boost::system::system_category());
		boost::asio::detail::throw_error(ec, "open");
	}

	try {
		struct ifreq ifr;
		memset(&ifr, 0, sizeof(ifr));
		ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
		int err;
		if ((err = ::ioctl(fd, TUNSETIFF, (void *) &ifr)) < 0) {
			boost::system::error_code ec;
			ec.assign(err, boost::system::system_category());
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
