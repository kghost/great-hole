#pragma once

#include <queue>

#include <boost/asio.hpp>

#include "endpoint.hpp"

class udp_mux_client : public std::enable_shared_from_this<udp_mux_client>, public endpoint {
	public:
		udp_mux_client(boost::asio::io_service& io_service, uint8_t id, boost::asio::ip::udp::endpoint peer);
		udp_mux_client(boost::asio::io_service& io_service, uint8_t id, boost::asio::ip::udp::endpoint peer, boost::asio::ip::udp::endpoint local);

		void async_start(fu2::unique_function<event> &&) override;
		void async_read(fu2::unique_function<read_handler> &&) override;
		void async_write(packet &&, fu2::unique_function<write_handler> &&) override;
	private:
		boost::asio::ip::udp::socket socket;
		const uint8_t id;
		const boost::asio::ip::udp::endpoint local;
		const boost::asio::ip::udp::endpoint peer;

		bool read_pending = false, write_pending = false;

		enum { none, opening, running, error } state = none;
};
