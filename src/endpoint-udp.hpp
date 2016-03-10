#ifndef ENDPOINT_UDP_H
#define ENDPOINT_UDP_H

#include <queue>

#include <boost/asio.hpp>

#include "endpoint.hpp"

class udp : public std::enable_shared_from_this<udp> {
	public:
		class udp_channel;

		udp(boost::asio::io_service& io_service);
		udp(boost::asio::io_service& io_service, boost::asio::ip::udp::endpoint bind);

		std::shared_ptr<endpoint> create_channel(boost::asio::ip::udp::endpoint const &peer);

	private:
		typedef std::map<boost::asio::ip::udp::endpoint, std::function<read_handler>> tm;
		// read_handler is holding the whole world, where we should break ref chain here
		std::weak_ptr<tm> reading_channel;
		boost::asio::ip::udp::socket socket;
		boost::asio::ip::udp::endpoint local;
		std::queue<std::tuple<boost::asio::ip::udp::endpoint, boost::asio::const_buffers_1 const, std::function<write_handler>>> write_queue;

		bool read_pending = false, write_pending = false;

		enum { none, running, error } state = none;
		void try_async_start(std::function<event> &&);
		void async_start(std::function<event> &&);
		void read(boost::asio::ip::udp::endpoint const &peer, std::function<read_handler> &&handler);
		void write(boost::asio::ip::udp::endpoint const &peer, boost::asio::const_buffers_1 const &b, std::function<write_handler> &&handler);
		void schedule_read(std::shared_ptr<tm> m);
		void schedule_write();
};

#endif /* end of include guard: ENDPOINT_UDP_H */
