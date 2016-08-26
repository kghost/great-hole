#include "config.h"

#include "endpoint-udp.hpp"

#include <cassert>
#include <boost/log/trivial.hpp>
#include <boost/asio/buffer.hpp>

class udp::udp_channel : public endpoint {
	public:
		udp_channel(std::shared_ptr<udp> parent, const boost::asio::ip::udp::endpoint &peer) : parent(parent), peer(peer) {}

		virtual void async_start(std::function<event> &&handler) {
			parent->try_async_start(std::move(handler));
		}

		virtual void async_read(std::function<read_handler> &&handler) {
			parent->read(peer, std::move(handler));
		}

		virtual void async_write(boost::asio::const_buffers_1 const &b, std::function<write_handler> &&handler) {
			parent->write(peer, b, std::move(handler));
		}

	private:
		std::shared_ptr<udp> parent;
		const boost::asio::ip::udp::endpoint peer;
};

std::shared_ptr<endpoint> udp::create_channel(boost::asio::ip::udp::endpoint const &peer) {
	return std::shared_ptr<endpoint>(new udp_channel(shared_from_this(), peer));
}

udp::udp(boost::asio::io_service& io_service) : socket(io_service), local(boost::asio::ip::udp::v4(), 0) {}
udp::udp(boost::asio::io_service& io_service, boost::asio::ip::udp::endpoint bind) : socket(io_service), local(bind) {}

class state_error : public gh::error_category {
	public:
		virtual const char * name() const noexcept { return "state error"; }
		virtual std::string message(int ev) const { return "udp state " + std::to_string(ev); }
} state_error;


void udp::try_async_start(std::function<event> &&handler) {
	switch (state) {
		case none:
			async_start(std::move(handler));
			break;
		case running:
			handler(gh::error_code());
			break;
		default:
			handler(gh::error_code(state, state_error));
			break;
	}
}

void udp::async_start(std::function<event> &&handler) {
	try {
		socket.open(boost::asio::ip::udp::v4());
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

void udp::read(boost::asio::ip::udp::endpoint const &peer, std::function<read_handler> &&handler) {
	if (state != running) return;
	std::shared_ptr<tm> m;
	if ((m = reading_channel.lock())) {
		assert(m->find(peer) == m->end());
	} else {
		m.reset(new tm);
		reading_channel = m;
	}
	m->insert(std::make_pair(peer, std::move(handler)));
	schedule_read(m);
}

void udp::schedule_read(std::shared_ptr<tm> m) {
	if (state != running || read_pending) return;
	read_pending = true;

	auto a = std::make_shared<std::array<char, 2048>>();
	auto p = packet{{a.get(), a->size()}, a};
	auto peer = std::make_shared<boost::asio::ip::udp::endpoint>();
	socket.async_receive_from(
		p.first, *peer,
		[me = shared_from_this(), p, m, peer](const gh::error_code& ec, std::size_t bytes_transferred) {
			me->read_pending = false;
			if (!ec) {
				auto h = m->find(*peer);
				if(h != m->end()) {
					auto handler = std::move(h->second);
					m->erase(h);
					handler(ec, packet{boost::asio::buffer(p.first, bytes_transferred), p.second});
				} else {
					BOOST_LOG_TRIVIAL(info) << "udp(" << &*me << ") packet from unknown peer: " << *peer;
				}
			} else {
				BOOST_LOG_TRIVIAL(error) << "udp(" << &*me << ") read error: " << ec.category().name() << ':' << ec.value();
			}
			if (!m->empty()) me->schedule_read(m);
		});
}

void udp::write(boost::asio::ip::udp::endpoint const &peer, boost::asio::const_buffers_1 const &b, std::function<write_handler> &&handler) {
	if (state != running) return;
	write_queue.push(std::make_tuple(peer, b, handler));
	schedule_write();
}

void udp::schedule_write() {
	if (state != running || write_pending) return;
	write_pending = true;

	auto &next = write_queue.front();
	auto peer = std::get<0>(next);
	auto b = std::get<1>(next);
	auto handler = std::move(std::get<2>(next));
	write_queue.pop();
	socket.async_send_to(b, peer, [me = shared_from_this(), handler](const gh::error_code& ec, std::size_t bytes_transferred) {
		me->write_pending = false;
		handler(ec, bytes_transferred);
		if (!me->write_queue.empty()) me->schedule_write();
	});
}
