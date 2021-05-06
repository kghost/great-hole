#include "config.h"

#include "logging.hpp"

#include <queue>
#include <boost/asio.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/frontend_requirements.hpp>
#include <boost/log/sinks/basic_sink_backend.hpp>
#include <boost/log/core/record_view.hpp>
#include <boost/log/core/core.hpp>

#include "endpoint.hpp"

class asio_log_backend :
	public boost::log::sinks::basic_formatted_sink_backend<char, boost::log::sinks::synchronized_feeding>
{
	public:
		explicit asio_log_backend(std::shared_ptr<endpoint_output> const &out) : impl(new detail(out)) {}

		void consume(boost::log::record_view const& rec, string_type const& log) {
			impl->write(log);
		}
	private:
		class detail : public std::enable_shared_from_this<detail> {
			public:
				explicit detail(std::shared_ptr<endpoint_output> const &out) : out(out) {}

				void write(std::string const &log) {
					q.push(log + '\n');
					schedule_write();
				}
			private:
				void schedule_write() {
					if (!write_pending && !q.empty()) {
						write_pending = true;

						auto p = packet{buffer(q.front()), nullptr};
						out->async_write(std::move(p), [me = shared_from_this()](const gh::error_code &ec, std::size_t bytes_transferred) {
							boost::asio::detail::throw_error(ec, "write log");
							me->write_pending = false;
							me->q.pop();
							me->schedule_write();
						});
					}
				}

				bool write_pending = false;
				std::shared_ptr<endpoint_output> out;
				std::queue<std::string> q;
		};

		std::shared_ptr<detail> impl;
};

void init_log(std::shared_ptr<endpoint_output> out) {
	typedef boost::log::sinks::synchronous_sink<asio_log_backend> text_sink;
	boost::log::core::get()->add_sink(boost::make_shared<text_sink>(boost::make_shared<asio_log_backend>(out)));
}
