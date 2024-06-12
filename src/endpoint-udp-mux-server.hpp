#pragma once

#include <queue>

#include <boost/asio.hpp>

#include "endpoint.hpp"
#include "scoped_flag.hpp"

class udp_mux_server : public std::enable_shared_from_this<udp_mux_server> {
	public:
		class channel;

		udp_mux_server(boost::asio::io_service& io_service);
		udp_mux_server(boost::asio::io_service& io_service, boost::asio::ip::udp::endpoint bind);

		std::shared_ptr<endpoint> create_channel(uint8_t id);

	private:
		typedef std::map<uint8_t, fu2::unique_function<read_handler>> tm;
		// read_handler is holding the whole world, where we should break ref chain here
		std::weak_ptr<tm> reading_channel;
		std::map<uint8_t, boost::asio::ip::udp::endpoint> peers;
		boost::asio::ip::udp::socket socket;
		boost::asio::ip::udp::endpoint local;
		std::queue<std::tuple<uint8_t, packet, fu2::unique_function<write_handler>>> write_queue;

		bool read_pending = false, write_pending = false;

		enum { none, running, error } state = none;
		void try_async_start(fu2::unique_function<event> &&);
		void async_start(fu2::unique_function<event> &&);
		void read(uint8_t id, fu2::unique_function<read_handler> &&handler);
		void write(uint8_t id, packet && b, fu2::unique_function<write_handler> &&handler);
		void schedule_read(std::shared_ptr<tm> m);
		void schedule_write(scoped_flag && write);

		bool started = false;
};
