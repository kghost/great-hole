#ifndef ENDPOINT_UDP_H
#define ENDPOINT_UDP_H

#include <cassert>

#include <boost/log/trivial.hpp>
#include <boost/asio.hpp>

#include "pipeline.hpp"

class udp : public std::enable_shared_from_this<udp> {
	public:
		class udp_channel : public endpoint {
			public:
				virtual ~udp_channel() {}

				virtual void async_read(std::function<read_handler> handler) {
					parent->read(peer, handler);
					
				}

				virtual void async_write(packet &p, std::function<read_handler> handler) {
					parent->write(peer, p, handler);
				}

			private:
				udp_channel(std::shared_ptr<udp> parent, const boost::asio::ip::udp::endpoint &peer) : parent(parent), peer(peer) {}

				std::shared_ptr<udp> parent;
				const boost::asio::ip::udp::endpoint peer;

				friend class udp;
		};

		udp(boost::asio::io_service& io_service) : socket(io_service) {
			socket.open(boost::asio::ip::udp::v4());
			socket.bind(boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));
		}

		udp(boost::asio::io_service& io_service, boost::asio::ip::udp::endpoint bind) : socket(io_service) {
			socket.open(boost::asio::ip::udp::v4());
			socket.bind(bind);
		}

		std::shared_ptr<udp_channel> create_channel(boost::asio::ip::udp::endpoint &&peer) {
			return std::shared_ptr<udp_channel>(new udp_channel(shared_from_this(), peer));
		}

	private:
		void read(boost::asio::ip::udp::endpoint peer, std::function<read_handler> handler);
		void write(const boost::asio::ip::udp::endpoint &peer, packet &p, std::function<write_handler> &handler);
		void schedule_read();
		void schedule_write();

		boost::asio::ip::udp::socket socket;

		std::map<boost::asio::ip::udp::endpoint, std::function<read_handler>> reading_channel;
		boost::asio::ip::udp::endpoint read_peer;
		std::queue<std::tuple<boost::asio::ip::udp::endpoint, packet, std::function<write_handler>>> write_queue;
};

#endif /* end of include guard: ENDPOINT_UDP_H */
