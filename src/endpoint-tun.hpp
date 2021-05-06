#ifndef ENDPOINT_TUN_H
#define ENDPOINT_TUN_H

#include <boost/asio.hpp>
#include <boost/asio/posix/basic_descriptor.hpp>

#include "endpoint.hpp"

class exec;
class tun : public std::enable_shared_from_this<tun>, public endpoint {
	public:
		tun(boost::asio::io_service &io_service, std::string const &name);
		tun(boost::asio::io_service &io_service, std::string const &name, std::shared_ptr<exec> e);

		void async_start(fu2::unique_function<event> &&) override;
		void async_read(fu2::unique_function<read_handler> &&) override;
		void async_write(packet &&, fu2::unique_function<write_handler> &&) override;
	private:
		boost::asio::posix::stream_descriptor s;

		const std::string name;
		std::shared_ptr<exec> e;

		bool started = false;
		gh::error_code started_ec;
};

#endif /* end of include guard: ENDPOINT_TUN_H */
