#include "endpoint-udp.hpp"

void udp::read(boost::asio::ip::udp::endpoint peer, std::function<read_handler> handler) {
	bool start_read = false;
	if (reading_channel.empty()) {
		start_read = true;
	}

	assert(reading_channel.find(peer) == reading_channel.end());
	reading_channel[peer] = handler;

	if (start_read) schedule_read();
}

void udp::schedule_read() {
	packet p;
	socket.async_receive_from(
		boost::asio::buffer(p.data.get(), p.sz), read_peer,
		[this, p](const boost::system::error_code& ec, std::size_t bytes_transferred) {
			if (!ec) {
				packet r(p);
				r.sz = bytes_transferred;
				auto h = reading_channel.find(read_peer);
				if(h != reading_channel.end()) {
					auto handler = h->second;
					reading_channel.erase(h);
					handler(ec, r);
				} else {
					BOOST_LOG_TRIVIAL(info) << "udp packet from unknown peer: " << read_peer;
				}
			} else {
				BOOST_LOG_TRIVIAL(info) << "udp read error: " << ec.category().name() << ':' << ec.value();
			}
			if (!reading_channel.empty()) schedule_read();
		});
}

void udp::write(const boost::asio::ip::udp::endpoint &peer, packet &p, std::function<write_handler> &handler) {
	bool start_write = false;
	if (write_queue.empty()) {
		start_write = true;
	}

	write_queue.push(std::make_tuple(peer, p, handler));

	if (start_write) schedule_write();
}

void udp::schedule_write() {
	auto next = write_queue.front();
	auto p = std::get<1>(next);
	auto handler = std::get<2>(next);
	write_queue.pop();
	socket.async_send_to(
		boost::asio::buffer(p.data.get(), p.sz), std::get<0>(next),
		[this, handler, p](const boost::system::error_code& ec, std::size_t bytes_transferred) {
			packet r(p);
			r.sz = bytes_transferred;
			handler(ec, r);
			if (!write_queue.empty()) schedule_read();
		});
}
