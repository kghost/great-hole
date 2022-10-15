#include "config.h"

#include "endpoint-udp-mux-server.hpp"

#include <cassert>
#include <boost/log/trivial.hpp>
#include <boost/asio/buffer.hpp>

#include "scoped_flag.hpp"

class udp_mux_server::channel : public endpoint {
	public:
		channel(std::shared_ptr<udp_mux_server> parent, uint8_t id) : parent(parent), id(id) {}

		void async_start(fu2::unique_function<event> &&handler) override {
			parent->try_async_start(std::move(handler));
		}

		void async_read(fu2::unique_function<read_handler> &&handler) override {
			parent->read(id, std::move(handler));
		}

		void async_write(packet && p, fu2::unique_function<write_handler> &&handler) override {
			parent->write(id, std::move(p), std::move(handler));
		}

	private:
		std::shared_ptr<udp_mux_server> parent;
		uint8_t id;
};

std::shared_ptr<endpoint> udp_mux_server::create_channel(uint8_t id) {
	return std::shared_ptr<endpoint>(new channel(shared_from_this(), id));
}

udp_mux_server::udp_mux_server(boost::asio::io_service& io_service) : socket(io_service), local(boost::asio::ip::udp::v6(), 0) {}
udp_mux_server::udp_mux_server(boost::asio::io_service& io_service, boost::asio::ip::udp::endpoint bind) : socket(io_service), local(bind) {}

void udp_mux_server::try_async_start(fu2::unique_function<event> &&handler) {
	switch (state) {
		case none:
			async_start(std::move(handler));
			break;
		case running:
			handler(gh::error_code());
			break;
		default:
			handler(gh::error_code{app_error_category::incorrect_state, app_error});
			break;
	}
}

void udp_mux_server::async_start(fu2::unique_function<event> &&handler) {
	try {
		socket.open(boost::asio::ip::udp::v6());
		socket.set_option(boost::asio::ip::v6_only(false));
		socket.bind(local);
		BOOST_LOG_TRIVIAL(info) << "udp_mux_server(" << this << ") bound at " << socket.local_endpoint();
		state = running;
		handler(gh::error_code());
	} catch (const gh::system_error &e) {
		BOOST_LOG_TRIVIAL(info) << "udp_mux_server(" << this << ") start failed: " << e.what();
		state = error;
		handler(e.code());
	}
}

void udp_mux_server::read(uint8_t id, fu2::unique_function<read_handler> &&handler) {
	if (state != running) return;
	std::shared_ptr<tm> m;
	if ((m = reading_channel.lock())) {
		assert(m->find(id) == m->end());
	} else {
		m.reset(new tm);
		reading_channel = m;
	}
	m->emplace(id, std::move(handler));
	schedule_read(m);
}

void udp_mux_server::schedule_read(std::shared_ptr<tm> m) {
	if (state != running || read_pending) return;
	read_pending = true;

	auto a = std::make_shared<std::array<uint8_t, 2048>>();
	auto p = packet{buffer(*a), a};
	auto buffer = boost::asio::mutable_buffer{p.first};
	auto peer_address = std::make_shared<boost::asio::ip::udp::endpoint>();
	socket.async_receive_from(
		buffer, *peer_address,
		[me = shared_from_this(), p{std::move(p)}, m, peer_address](const gh::error_code& ec, std::size_t bytes_transferred) mutable {
			me->read_pending = false;
			if (!ec) {
				auto & buffer = p.first;

				assert(bytes_transferred <= buffer.capacity - buffer.offset);
				buffer.length = bytes_transferred;

				if (bytes_transferred < 1) return;
				uint8_t id= buffer.data[buffer.offset];
				buffer.offset += 1;
				buffer.length -= 1;
				me->peers[id] = *peer_address; // learn peer address

				auto h = m->find(id);
				if(h != m->end()) {
					auto handler = std::move(h->second);
					m->erase(h);
					handler(ec, std::move(p));
				} else {
					BOOST_LOG_TRIVIAL(info) << "udp_mux_server(" << &*me << ") packet from unknown peer: " << id;
				}
			} else {
				BOOST_LOG_TRIVIAL(error) << "udp_mux_server(" << &*me << ") read error: " << ec.category().name() << ':' << ec.value();
			}
			if (!m->empty()) me->schedule_read(m);
		});
}

void udp_mux_server::write(uint8_t id, packet && p, fu2::unique_function<write_handler> &&handler) {
	if (state != running) return;
	write_queue.push(std::make_tuple(id, std::move(p), std::move(handler)));
	schedule_write();
}

void udp_mux_server::schedule_write() {
	if (state != running || write_pending) return;

	scoped_flag write(write_pending);
	auto &next = write_queue.front();
	auto id = std::get<0>(next);
	auto p = std::move(std::get<1>(next));
	auto handler = std::move(std::get<2>(next));
	write_queue.pop();

	auto peer_iter = peers.find(id);
	if (peer_iter == peers.end()) {
		handler(gh::error_code{app_error_category::invalid_packet_session, app_error}, 0);
		return;
	}

	auto & buffer = p.first;
	if (buffer.offset < 1) {
		handler(gh::error_code{app_error_category::invalid_packet_reserved, app_error}, 0);
		return;
	}

	buffer.offset -= 1;
	buffer.length += 1;
	buffer.data[buffer.offset] = id;

	socket.async_send_to(boost::asio::const_buffer{p.first}, peer_iter->second, [me = shared_from_this(), handler{std::move(handler)}, write{std::move(write)}](const gh::error_code& ec, std::size_t bytes_transferred) mutable {
		handler(ec, bytes_transferred);
		if (!me->write_queue.empty()) me->schedule_write();
	});
}
