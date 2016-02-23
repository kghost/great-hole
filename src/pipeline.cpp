#include "pipeline.hpp"

#include "packet.hpp"
#include "filter.hpp"
#include "endpoint.hpp"

void pipeline::process(packet &p) {
	packet t(std::move(p));
	for (auto i : filters) {
		t = std::move(i->pipe(t));
	}
	schedule_write(p);
}

void pipeline::schedule_read() {
	if (started && !paused && !read_pending) {
		read_pending = true;
		in->async_read([this](boost::system::error_code ec, packet &p) { this->read_callback(ec, p); });
	}
}

void pipeline::read_callback(boost::system::error_code ec, packet &p) {
	read_pending = false;
	fc.after_read();
	if (!write_pending) {
		process(p);
	} else {
		buffer.push(std::move(p));
	}
	schedule_read();
}

void pipeline::schedule_write(packet &p) {
	assert(!write_pending);
	write_pending = true;
	out->async_write(p, [this](boost::system::error_code ec, packet &p) { this->write_callback(ec, p); });
}

void pipeline::write_callback(boost::system::error_code ec, packet &p) {
	write_pending = false;
	fc.after_write();
	if (!buffer.empty()) {
		packet next = std::move(buffer.front());
		buffer.pop();
		process(next);
	}
}
