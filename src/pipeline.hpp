#ifndef PIPELINE_H
#define PIPELINE_H

#include <memory>
#include <queue>

#include <boost/system/error_code.hpp>

#include "flowcontrol.hpp"

class packet;
class filter;
class endpoint;
class pipeline {
	public:
		pipeline(std::shared_ptr<endpoint> in, std::vector<std::shared_ptr<filter>> const &filters, std::shared_ptr<endpoint> out) :
			fc(this), in(in), filters(filters), out(out),
			started(false), paused(false), read_pending(false), write_pending(false) {}

	public:
		void start() {
			if (started) return;
			started = true;
			schedule_read();
		}

		void stop() {
			if (!started) return;
			started = false;
		}

		void pause() {
			if (paused) return;
			paused = true;
		}

		void resume() {
			if (!paused) return;
			paused = false;
			schedule_read();
		}

		int size() { return buffer.size(); }

	private:
		void process(packet &p);

		void schedule_read();
		void read_callback(boost::system::error_code ec, packet &p);
		void schedule_write(packet &p);
		void write_callback(boost::system::error_code ec, packet &p);

		bool started;
		bool paused;
		bool read_pending;
		bool write_pending;

		std::shared_ptr<endpoint> in;
		std::shared_ptr<endpoint> out;
		std::vector<std::shared_ptr<filter>> filters;
		std::queue<packet> buffer;
		flow_control<pipeline> fc;
};

#endif /* end of include guard: PIPELINE_H */
