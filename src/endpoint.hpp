#ifndef ENDPOINT_H
#define ENDPOINT_H

#include <functional>

#include "error-code.hpp"
#include "packet.hpp"

using event = void (gh::error_code const &);
using read_handler = void (gh::error_code const &, packet &&);
using write_handler = void (gh::error_code const &, std::size_t);

class endpoint_input {
	public:
		virtual ~endpoint_input() = 0;
		virtual void async_start(std::move_only_function<event> &&) = 0;
		virtual void async_read(std::move_only_function<read_handler> &&) = 0;
};

class endpoint_output {
	public:
		virtual ~endpoint_output() = 0;
		virtual void async_start(std::move_only_function<event> &&) = 0;
		virtual void async_write(packet &&, std::move_only_function<write_handler> &&) = 0;
};

class endpoint : public endpoint_input, public endpoint_output {};

template<typename base>
class endpoint_skip_start : public base {
	public:
		virtual void async_start(std::move_only_function<event> &&handler) { handler(gh::error_code()); }
};

#endif /* end of include guard: ENDPOINT_H */
