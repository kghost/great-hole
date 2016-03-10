#ifndef ENDPOINT_EXEC_H
#define ENDPOINT_EXEC_H

#include "endpoint.hpp"

#include "util-exec.hpp"

class endpoint_exec : public endpoint {
	public:
		virtual void async_start(std::function<event> &&);
		virtual void async_read(std::function<read_handler> &&);
		virtual void async_write(boost::asio::const_buffers_1 const &, std::function<write_handler> &&);

	private:
		exec e;
};

#endif /* end of include guard: ENDPOINT_EXEC_H */
