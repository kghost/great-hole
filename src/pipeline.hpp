#ifndef PIPELINE_H
#define PIPELINE_H

#include <memory>
#include <queue>

#include "packet.hpp"
#include "flowcontrol.hpp"

class filter;
class endpoint_input;
class endpoint_output;
class endpoint;
class pipeline : public std::enable_shared_from_this<pipeline> {
	public:
		pipeline(std::shared_ptr<endpoint_input> in, std::vector<std::shared_ptr<filter>> const &filters, std::shared_ptr<endpoint_output> out);
		~pipeline() { stop(); }

		void start();
		void stop();
		void pause();
		void resume();

		int size() { return buffer.size(); }

	private:
		void process(packet &&p);

		void schedule_read();
		void schedule_write(packet &&p);

		enum { none, starting, running, paused, stopped, error } state = none;
		bool read_pending = false;
		bool write_pending = false;

		std::shared_ptr<endpoint_input> in;
		std::shared_ptr<endpoint_output> out;
		std::vector<std::shared_ptr<filter>> filters;
		std::queue<packet> buffer;
		flow_control<pipeline> fc;
};

#endif /* end of include guard: PIPELINE_H */
