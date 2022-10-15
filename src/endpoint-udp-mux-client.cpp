#include "config.h"

#include "endpoint-udp-mux-client.hpp"

#include <cassert>
#include <boost/log/trivial.hpp>
#include <boost/asio/buffer.hpp>

udp_mux_client::udp_mux_client(boost::asio::io_service& io_service, uint8_t id, boost::asio::ip::udp::endpoint peer) : socket(io_service), id(id), peer(peer), local(boost::asio::ip::udp::v6(), 0) {}
udp_mux_client::udp_mux_client(boost::asio::io_service& io_service, uint8_t id, boost::asio::ip::udp::endpoint peer, boost::asio::ip::udp::endpoint local) : socket(io_service), id(id), peer(peer), local(local) {}

void udp_mux_client::async_start(fu2::unique_function<event> &&handler) {
	switch (state) {
		case none:
			state = opening;
			try {
				socket.open(boost::asio::ip::udp::v6());
				socket.set_option(boost::asio::ip::v6_only(false));
				socket.bind(local);
				BOOST_LOG_TRIVIAL(info) << "udp_mux_client(" << this << ") bound at " << socket.local_endpoint();
			} catch (const gh::system_error &e) {
				BOOST_LOG_TRIVIAL(info) << "udp_mux_client(" << this << ") start failed: " << e.what();
				state = error;
				handler(e.code());
			}

			socket.async_connect(peer, [me = shared_from_this(), handler{std::move(handler)}](const gh::error_code& ec) mutable {
				if (!ec) me->state = running;
				handler(gh::error_code());
			});
			break;
		case opening:
			BOOST_LOG_TRIVIAL(info) << "udp_mux_client(" << this << ") start opening";
			handler(gh::error_code());
			break;
		case running:
			handler(gh::error_code());
			break;
		default:
			handler(gh::error_code{app_error_category::incorrect_state, app_error});
			break;
	}
}

void udp_mux_client::async_read(fu2::unique_function<read_handler> &&handler) {
	auto a = std::make_shared<std::array<uint8_t, 2048>>();
	auto p = packet{buffer(*a), a};
	auto buffer = boost::asio::mutable_buffer{p.first};
	socket.async_receive(buffer, [me = shared_from_this(), p{std::move(p)}, handler{std::move(handler)}](const gh::error_code& ec, std::size_t bytes_transferred) mutable {
		if (!ec) {
			auto & buffer = p.first;

			assert(bytes_transferred <= buffer.capacity - buffer.offset);
			buffer.length = bytes_transferred;

			if (bytes_transferred < 1) {
				handler(gh::error_code{app_error_category::invalid_packet_size, app_error}, std::move(p));
				return;
			}

			uint8_t channel_id = buffer.data[buffer.offset];
			if (channel_id != me->id) {
				handler(gh::error_code{app_error_category::invalid_packet_session, app_error}, std::move(p));
				return;
			}

			buffer.offset += 1;
			buffer.length -= 1;
		}
		handler(ec, std::move(p));
	});
}

void udp_mux_client::async_write(packet && p, fu2::unique_function<write_handler> && handler) {
	if (state != running) return;
	assert(!write_pending);
	write_pending = true;

	auto & buffer = p.first;
	if (buffer.offset < 1) {
		handler(gh::error_code{app_error_category::invalid_packet_reserved, app_error}, 0);
		return;
	}

	buffer.offset -= 1;
	buffer.length += 1;
	buffer.data[buffer.offset] = id;

	socket.async_send(boost::asio::const_buffer{buffer}, [me = shared_from_this(), handler{std::move(handler)}](const gh::error_code& ec, std::size_t bytes_transferred) mutable {
		me->write_pending = false;
		handler(ec, bytes_transferred);
	});
}
