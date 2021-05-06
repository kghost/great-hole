#include "config.h"

#include "util-console.hpp"

#include <boost/asio.hpp>

#include "endpoint.hpp"

class input : public endpoint_skip_start<endpoint_input> {
	public:
		explicit input(boost::asio::io_service &io_service, decltype(STDERR_FILENO) f) : s(io_service, f) {}

		virtual void async_read(fu2::unique_function<read_handler> &&handler) {
			auto a = std::make_shared<std::array<uint8_t, 2048>>();
			auto p = packet{buffer(*a), a};
			auto buffer = boost::asio::mutable_buffer{p.first};
			s.async_read_some(buffer, [p{std::move(p)}, handler = std::move(handler)](const gh::error_code& ec, std::size_t bytes_transferred) mutable {
				if (!ec) {
					assert(bytes_transferred <= p.first.capacity - p.first.offset);
					p.first.length = bytes_transferred;
				}
				handler(ec, std::move(p));
			});
		}

	private:
		boost::asio::posix::stream_descriptor s;
};

class output : public endpoint_skip_start<endpoint_output> {
	public:
		explicit output(boost::asio::io_service &io_service, decltype(STDERR_FILENO) f) : s(io_service, f) {}

		void async_write(packet && p, fu2::unique_function<write_handler> && handler) override {
			boost::asio::async_write(s, boost::asio::const_buffer{p.first}, std::move(handler));
		}

	private:
		boost::asio::posix::stream_descriptor s;
};

static std::weak_ptr<endpoint_input> in;
static std::weak_ptr<endpoint_output> out;
static std::weak_ptr<endpoint_output> err;

std::shared_ptr<endpoint_input> get_cin(boost::asio::io_service &io_service) {
	auto p = in.lock();
	if (p) {
		return p;
	} else {
		auto o = std::make_shared<input>(io_service, STDIN_FILENO);
		in = o;
		return o;
	}
}

std::shared_ptr<endpoint_output> get_cout(boost::asio::io_service &io_service) {
	auto p = out.lock();
	if (p) {
		return p;
	} else {
		auto o = std::make_shared<output>(io_service, STDOUT_FILENO);
		out = o;
		return o;
	}
}

std::shared_ptr<endpoint_output> get_cerr(boost::asio::io_service &io_service) {
	auto p = err.lock();
	if (p) {
		return p;
	} else {
		auto o = std::make_shared<output>(io_service, STDERR_FILENO);
		err = o;
		return o;
	}
}
