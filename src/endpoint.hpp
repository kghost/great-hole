#ifndef ENDPOINT_H
#define ENDPOINT_H

#include <boost/function.hpp>

#include "error-code.hpp"
#include "packet.hpp"

typedef void (event)(gh::error_code const &);
typedef void (read_handler)(gh::error_code const &, packet &&);
typedef void (write_handler)(gh::error_code const &, std::size_t);

class endpoint_input {
	public:
		virtual ~endpoint_input() = 0;
		virtual void async_start(std::function<event> &&) = 0;
		virtual void async_read(std::function<read_handler> &&) = 0;
};

class endpoint_output {
	public:
		virtual ~endpoint_output() = 0;
		virtual void async_start(std::function<event> &&) = 0;
		virtual void async_write(boost::asio::const_buffers_1 const &, std::function<write_handler> &&) = 0;
};

class endpoint : public endpoint_input, public endpoint_output {};

template<typename base>
class endpoint_skip_start : public base {
	public:
		virtual void async_start(std::function<event> &&handler) { handler(gh::error_code()); }
};

#endif /* end of include guard: ENDPOINT_H */
