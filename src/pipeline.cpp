#include "config.h"

#include "pipeline.hpp"

#include <memory>
#include <boost/asio/buffer.hpp>
#include <boost/log/trivial.hpp>

#include "filter.hpp"
#include "endpoint.hpp"
#include "scoped_flag.hpp"

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
	if (state != running && state != paused) {
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

void pipeline::process(scoped_flag && write, packet &&p) {
	for (auto i : filters) {
		p = i->pipe(std::move(p));
	}
	schedule_write(std::move(write), std::move(p));
}

void pipeline::process_queue(scoped_flag && write) {
	if (!buffers.empty()) {
		auto next = std::move(buffers.front());
		buffers.pop();
		process(std::move(write), std::move(next));
	}
}

void pipeline::schedule_read() {
	if (state == running && !read_pending) {
		schedule_read(scoped_flag(read_pending));
	}
}

void pipeline::schedule_read(scoped_flag && read) {
	if (state == running) {
		in->async_read([this, me = shared_from_this(), read{std::move(read)}](const gh::error_code &ec, packet &&p) mutable {
			//fc.after_read();
			if (!ec) {
				if (!write_pending) {
					process(scoped_flag(write_pending), std::move(p));
				} else {
					buffers.push(std::move(p));
				}
			} else {
				BOOST_LOG_TRIVIAL(error) << "pipeline(" << this << ") read error: " << ec.message();
				stop();
			}
			schedule_read(std::move(read));
		});
	}
}

void pipeline::schedule_write(scoped_flag && write, packet &&p) {
	if (state == running || state == paused) {
		auto buffer = boost::asio::const_buffer(p.first);
		auto storage = p.second;
		out->async_write(std::move(p), [me = shared_from_this(), storage, write{std::move(write)}](const gh::error_code &ec, std::size_t bytes_transferred) mutable {
			//me->fc.after_write();
			if (ec) {
				if (me->is_critical(ec)) {
					BOOST_LOG_TRIVIAL(error) << "pipeline(" << &*me << ") write error: " << ec.message();
					me->stop();
				} else {
					BOOST_LOG_TRIVIAL(warning) << "pipeline(" << &*me << ") write error (non-critical): " << ec.message();
				}
			}
			me->process_queue(std::move(write));
		});
	}
}

bool pipeline::is_critical(const gh::error_code &ec) {
	if (ec.category() == boost::system::system_category()) {
		switch(ec.value()) {
			case boost::system::errc::invalid_argument:
			case boost::system::errc::io_error:
			case boost::system::errc::connection_refused:
				return false;
			default: 
				return true;
		}
	} else if (ec.category() == app_error) {
		switch(ec.value()) {
			case app_error_category::invalid_packet_session:
			case app_error_category::invalid_packet_size:
				return false;
			default: 
				return true;
		}
	} else {
		return true;
	}
}
