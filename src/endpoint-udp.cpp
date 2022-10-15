#include "config.h"

#include "endpoint-udp.hpp"

#include <cassert>
#include <boost/log/trivial.hpp>
#include <boost/asio/buffer.hpp>

class udp::udp_channel : public endpoint {
	public:
		udp_channel(std::shared_ptr<udp> parent, const boost::asio::ip::udp::endpoint &peer) : parent(parent), peer(peer) {}

		void async_start(fu2::unique_function<event> &&handler) override {
			parent->try_async_start(std::move(handler));
		}

		void async_read(fu2::unique_function<read_handler> &&handler) override {
			parent->read(peer, std::move(handler));
		}

		void async_write(packet && p, fu2::unique_function<write_handler> &&handler) override {
			parent->write(peer, std::move(p), std::move(handler));
		}

	private:
		std::shared_ptr<udp> parent;
		const boost::asio::ip::udp::endpoint peer;
};

std::shared_ptr<endpoint> udp::create_channel(boost::asio::ip::udp::endpoint const &peer) {
	return std::shared_ptr<endpoint>(new udp_channel(shared_from_this(), peer));
}

udp::udp(boost::asio::io_service& io_service) : socket(io_service), local(boost::asio::ip::udp::v6(), 0) {}
udp::udp(boost::asio::io_service& io_service, boost::asio::ip::udp::endpoint bind) : socket(io_service), local(bind) {}

void udp::try_async_start(fu2::unique_function<event> &&handler) {
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

void udp::async_start(fu2::unique_function<event> &&handler) {
	try {
		socket.open(boost::asio::ip::udp::v6());
		socket.set_option(boost::asio::ip::v6_only(false));
		socket.bind(local);
		BOOST_LOG_TRIVIAL(info) << "udp(" << this << ") bound at " << socket.local_endpoint();
		state = running;
		handler(gh::error_code());
	} catch (const gh::system_error &e) {
		BOOST_LOG_TRIVIAL(info) << "udp(" << this << ") start failed: " << e.what();
		state = error;
		handler(e.code());
	}
}

void udp::read(boost::asio::ip::udp::endpoint const &peer, fu2::unique_function<read_handler> &&handler) {
	if (state != running) return;
	std::shared_ptr<tm> m;
	if ((m = reading_channel.lock())) {
		assert(m->find(peer) == m->end());
	} else {
		m.reset(new tm);
		reading_channel = m;
	}
	m->emplace(peer, std::move(handler));
	schedule_read(m);
}

void udp::schedule_read(std::shared_ptr<tm> m) {
	if (state != running || read_pending) return;
	read_pending = true;

	auto a = std::make_shared<std::array<uint8_t, 2048>>();
	auto p = packet{buffer(*a), a};
	auto buffer = boost::asio::mutable_buffer{p.first};
	auto peer = std::make_shared<boost::asio::ip::udp::endpoint>();
	socket.async_receive_from(
		buffer, *peer,
		[me = shared_from_this(), p{std::move(p)}, m, peer](const gh::error_code& ec, std::size_t bytes_transferred) mutable {
			me->read_pending = false;
			if (!ec) {
				assert(bytes_transferred <= p.first.capacity - p.first.offset);
				p.first.length = bytes_transferred;

				auto h = m->find(*peer);
				if(h != m->end()) {
					auto handler = std::move(h->second);
					m->erase(h);
					handler(ec, std::move(p));
				} else {
					BOOST_LOG_TRIVIAL(info) << "udp(" << &*me << ") packet from unknown peer: " << *peer;
				}
			} else {
				BOOST_LOG_TRIVIAL(error) << "udp(" << &*me << ") read error: " << ec.category().name() << ':' << ec.value();
			}
			if (!m->empty()) me->schedule_read(m);
		});
}

void udp::write(boost::asio::ip::udp::endpoint const &peer, packet && p, fu2::unique_function<write_handler> &&handler) {
	if (state != running) return;
	write_queue.push(std::make_tuple(peer, std::move(p), std::move(handler)));
	schedule_write();
}

void udp::schedule_write() {
	if (state != running || write_pending) return;
	write_pending = true;

	auto &next = write_queue.front();
	auto peer = std::get<0>(next);
	auto p = std::move(std::get<1>(next));
	auto handler = std::move(std::get<2>(next));
	write_queue.pop();
	socket.async_send_to(boost::asio::const_buffer{p.first}, peer, [me = shared_from_this(), handler{std::move(handler)}](const gh::error_code& ec, std::size_t bytes_transferred) mutable {
		me->write_pending = false;
		handler(ec, bytes_transferred);
		if (!me->write_queue.empty()) me->schedule_write();
	});
}
