#ifndef PIPELINE_H
#define PIPELINE_H

#include <memory>
#include <queue>

#include "packet.hpp"
#include "error-code.hpp"
#include "flowcontrol.hpp"

class filter;
class endpoint_input;
class endpoint_output;
class scoped_flag;

class pipeline : public std::enable_shared_from_this<pipeline> {
	public:
		pipeline(std::shared_ptr<endpoint_input> in, std::vector<std::shared_ptr<filter>> const &filters, std::shared_ptr<endpoint_output> out);
		~pipeline() { stop(); }

		void start();
		void stop();
		void pause();
		void resume();

		int size() { return buffers.size(); }

	private:
		bool is_critical(const gh::error_code &ec);

		void process(scoped_flag && write, packet &&p);
		void process_queue(scoped_flag && write);

		void schedule_read();
		void schedule_read(scoped_flag && read);
		void schedule_write(scoped_flag && write, packet &&p);

		enum { none, starting, running, paused, stopped, error } state = none;
		bool read_pending = false;
		bool write_pending = false;

		std::shared_ptr<endpoint_input> in;
		std::shared_ptr<endpoint_output> out;
		std::vector<std::shared_ptr<filter>> filters;
		std::queue<packet> buffers;
		flow_control<pipeline> fc;
};

#endif /* end of include guard: PIPELINE_H */
