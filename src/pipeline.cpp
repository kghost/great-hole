#include "config.h"

#include "pipeline.hpp"

#include <memory>
#include <boost/asio/buffer.hpp>
#include <boost/log/trivial.hpp>

#include "filter.hpp"
#include "endpoint.hpp"

pipeline::pipeline(std::shared_ptr<endpoint_input> in, std::vector<std::shared_ptr<filter>> const &filters, std::shared_ptr<endpoint_output> out) :
	fc(this), in(in), filters(filters), out(out) {}

void pipeline::start() {
	if (state != none) {
		BOOST_LOG_TRIVIAL(error) << "pipeline(" << this << ") already started: " << state;
		return;
	}
	BOOST_LOG_TRIVIAL(info) << "pipeline(" << this << ") starting";
	state = starting;
	std::shared_ptr<char> result(new char, [me = shared_from_this()] (char *r) {
		std::unique_ptr<char> _u(r);
		if (me->state == starting) {
			BOOST_LOG_TRIVIAL(info) << "pipeline " << &*me << " started";
			me->state = running;
			me->schedule_read();
		}
	});
	in->async_start([result, me = shared_from_this()] (const gh::error_code &ec) {
		if (ec) {
			BOOST_LOG_TRIVIAL(error) << "pipeline(" << &*me << ") input start error: " << ec.message();
			me->state = error;
		}
	});
	out->async_start([result, me = shared_from_this()] (const gh::error_code &ec) {
		if (ec) {
			BOOST_LOG_TRIVIAL(error) << "pipeline(" << &*me << ") output start error: " << ec.message();
			me->state = error;
		}
	});
}

void pipeline::stop() {
	if (state != running || state != paused) {
		BOOST_LOG_TRIVIAL(error) << "pipeline(" << this << ") is not running: " << state;
		return;
	}
	state = stopped;
}

void pipeline::pause() {
	if (state != running) {
		BOOST_LOG_TRIVIAL(error) << "pipeline(" << this << ") is not running: " << state;
		return;
	}
	state = paused;
}

void pipeline::resume() {
	if (state != paused) {
		BOOST_LOG_TRIVIAL(error) << "pipeline(" << this << ") is not paused: " << state;
		return;
	}
	state = running;
	schedule_read();
}

void pipeline::process(packet &&p) {
	for (auto i : filters) {
		p = i->pipe(std::move(p));
	}
	schedule_write(std::move(p));
}

void pipeline::schedule_read() {
	if (state == running && !read_pending) {
		read_pending = true;
		in->async_read([this, me = shared_from_this()](const gh::error_code &ec, packet &&p) {
			read_pending = false;
			fc.after_read();
			if (!ec) {
				if (!write_pending) {
					process(std::move(p));
				} else {
					buffer.push(std::move(p));
				}
			} else {
				BOOST_LOG_TRIVIAL(error) << "pipeline(" << this << ") read error: " << ec.message();
				stop();
			}
			schedule_read();
		});
	}
}

void pipeline::schedule_write(packet &&p) {
	if (state == running || state == paused) {
		assert(!write_pending);
		write_pending = true;
		out->async_write(boost::asio::const_buffers_1(p.first), [this, me = shared_from_this()](const gh::error_code &ec, std::size_t bytes_transferred) {
			write_pending = false;
			fc.after_write();
			if (ec) {
				BOOST_LOG_TRIVIAL(error) << "pipeline(" << this << ") write error: " << ec.message();
				stop();
			}
			if (!buffer.empty()) {
				auto next = std::move(buffer.front());
				buffer.pop();
				process(std::move(next));
			}
		});
	}
}
