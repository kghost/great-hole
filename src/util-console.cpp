#include "config.h"

#include "util-console.hpp"

#include <boost/asio.hpp>

#include "endpoint.hpp"

class input : public endpoint_skip_start<endpoint_input> {
	public:
		explicit input(boost::asio::io_service &io_service, decltype(STDERR_FILENO) f) : s(io_service, f) {}

		virtual void async_read(std::function<read_handler> &&handler) {
			auto a = std::make_shared<std::array<char, 2048>>();
			auto p = packet{{a.get(), a->size()}, a};
			s.async_read_some(p.first, [p, handler = std::move(handler)](const gh::error_code& ec, std::size_t bytes_transferred) {
				handler(ec, packet{boost::asio::buffer(p.first, bytes_transferred), p.second});
			});
		}

	private:
		boost::asio::posix::stream_descriptor s;
};

class output : public endpoint_skip_start<endpoint_output> {
	public:
		explicit output(boost::asio::io_service &io_service, decltype(STDERR_FILENO) f) : s(io_service, f) {}

		virtual void async_write(boost::asio::const_buffers_1 const &b, std::function<write_handler> &&handler) {
			boost::asio::async_write(s, b, handler);
		}

	private:
		boost::asio::posix::stream_descriptor s;
};

static std::shared_ptr<endpoint_input> in;
static std::shared_ptr<endpoint_output> out;
static std::shared_ptr<endpoint_output> err;

std::shared_ptr<endpoint_input> get_cin(boost::asio::io_service &io_service) {
	if (!in) in.reset(new input(io_service, STDIN_FILENO));
	return in;
}

std::shared_ptr<endpoint_output> get_cout(boost::asio::io_service &io_service) {
	if (!out) out.reset(new output(io_service, STDOUT_FILENO));
	return out;
}

std::shared_ptr<endpoint_output> get_cerr(boost::asio::io_service &io_service) {
	if (!err) err.reset(new output(io_service, STDOUT_FILENO));
	return err;
}
