#include <cassert>
#include <boost/log/trivial.hpp>

#include "endpoint-udp.hpp"

void udp::read(boost::asio::ip::udp::endpoint peer, std::function<read_handler> handler) {
	assert(reading_channel.find(peer) == reading_channel.end());
	reading_channel[peer] = handler;
	schedule_read();
}

void udp::schedule_read() {
	if (read_pending) return;
	read_pending = true;

	std::shared_ptr<packet> pp(new packet);
	auto me = shared_from_this();
	socket.async_receive_from(
		boost::asio::buffer(pp->data.get(), pp->sz), read_peer,
		[me, pp](const boost::system::error_code& ec, std::size_t bytes_transferred) {
			me->read_pending = false;
			if (!ec) {
				pp->sz = bytes_transferred;
				auto h = me->reading_channel.find(me->read_peer);
				if(h != me->reading_channel.end()) {
					auto handler = h->second;
					me->reading_channel.erase(h);
					handler(ec, *pp);
				} else {
					BOOST_LOG_TRIVIAL(info) << "udp packet from unknown peer: " << me->read_peer;
				}
			} else {
				BOOST_LOG_TRIVIAL(info) << "udp read error: " << ec.category().name() << ':' << ec.value();
			}
			me->schedule_read();
		});
}

void udp::write(const boost::asio::ip::udp::endpoint &peer, packet &p, std::function<write_handler> &handler) {
	write_queue.push(std::make_tuple(peer, std::move(p), handler));
	schedule_write();
}

void udp::schedule_write() {
	if (write_pending) return;
	write_pending = true;

	auto &next = write_queue.front();
	std::shared_ptr<packet> pp(new packet(std::move(std::get<1>(next))));
	auto handler = std::get<2>(next);
	write_queue.pop();
	auto me = shared_from_this();
	socket.async_send_to(
		boost::asio::buffer(pp->data.get(), pp->sz), std::get<0>(next),
		[me, handler, pp](const boost::system::error_code& ec, std::size_t bytes_transferred) {
			me->write_pending = false;
			pp->sz = bytes_transferred;
			handler(ec, *pp);
			if (!me->write_queue.empty()) me->schedule_read();
		});
}
