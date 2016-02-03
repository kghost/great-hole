#ifndef PIPELINE_ENDPOINT_H
#define PIPELINE_ENDPOINT_H

#include "pipeline.hpp"

template<typename IoObject>
class endpoint_object : public endpoint {
	public:
		endpoint_object(std::shared_ptr<IoObject> io) : o(io) {}

		virtual void async_read(std::function<read_handler> handler) {
			packet p;
			this->o->async_receive(
				boost::asio::buffer(p.data.get(), p.sz),
				[handler, p](const boost::system::error_code& ec, std::size_t bytes_transferred) {
					packet r(p);
					r.sz = bytes_transferred;
					handler(ec, r);
				});
		}

		virtual void async_write(packet &p, std::function<read_handler> handler) {
			this->o->async_send(
				boost::asio::buffer(p.data.get(), p.sz),
				[handler, p](const boost::system::error_code& ec, std::size_t bytes_transferred) {
					packet r(p);
					r.sz = bytes_transferred;
					handler(ec, r);
				});
		}

	protected:
		std::shared_ptr<IoObject> o;
};

#endif /* end of include guard: PIPELINE_ENDPOINT_H */
