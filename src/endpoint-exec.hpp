#ifndef ENDPOINT_EXEC_H
#define ENDPOINT_EXEC_H

#include "endpoint.hpp"

#include "util-exec.hpp"

class endpoint_exec : public endpoint {
	public:
		virtual void async_start(fu2::unique_function<event> &&);
		virtual void async_read(fu2::unique_function<read_handler> &&);
		virtual void async_write(boost::asio::const_buffer const &, fu2::unique_function<write_handler> &&);

	private:
		exec e;
};

#endif /* end of include guard: ENDPOINT_EXEC_H */
