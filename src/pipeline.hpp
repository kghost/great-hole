#ifndef PIPELINE_H
#define PIPELINE_H

#include <queue>

#include <boost/system/error_code.hpp>

#include "packet.hpp"
#include "flowcontrol.hpp"

typedef void (read_handler)(boost::system::error_code, packet);
typedef void (write_handler)(boost::system::error_code, packet);

class endpoint {
	public:
		virtual ~endpoint() = 0;
		virtual void async_read(std::function<read_handler>) = 0;
		virtual void async_write(packet &p, std::function<write_handler>) = 0;
};

class filter {
	public:
		virtual ~filter() = 0;
		virtual packet pipe(packet &p) = 0;
};

namespace detail {
	static void init_filters(std::vector<std::shared_ptr<filter>> &filters) {}

	template<typename F1, typename... Filters>
	void init_filters(std::vector<std::shared_ptr<filter>> &filters, std::shared_ptr<F1> f, std::shared_ptr<Filters>... fs) {
		filters.push_back(f);
		init_filters(filters, fs...);
	}
}

class pipeline {
	public:
		template<typename... Filters>
		static std::shared_ptr<pipeline>
		create_pipeline(std::shared_ptr<endpoint> in, std::shared_ptr<endpoint> out, std::shared_ptr<Filters> ... fs) {
			std::vector<std::shared_ptr<filter>> filters;
			detail::init_filters(filters, fs...);
			return std::shared_ptr<pipeline>(new pipeline(in, filters, out));
		}

	private:
		pipeline(std::shared_ptr<endpoint> in, std::vector<std::shared_ptr<filter>> &filters, std::shared_ptr<endpoint> out) :
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
		void process(packet &p) {
			for (auto i : filters) {
				p = i->pipe(p);
			}
			schedule_write(p);
		}

		void schedule_read() {
			if (started && !paused && !read_pending) {
				read_pending = true;
				in->async_read([this](boost::system::error_code ec, packet p) { this->read_callback(ec, p); });
			}
		}

		void read_callback(boost::system::error_code ec, packet p) {
			read_pending = false;
			fc.after_read();
			if (!write_pending) {
				process(p);
			} else {
				buffer.push(p);
			}
			schedule_read();
		}

		void schedule_write(packet p) {
			assert(!write_pending);
			write_pending = true;
			out->async_write(p, [this](boost::system::error_code ec, packet p) { this->write_callback(ec, p); });
		}

		void write_callback(boost::system::error_code ec, packet p) {
			write_pending = false;
			fc.after_write();
			if (!buffer.empty()) {
				packet next = buffer.front();
				buffer.pop();
				process(next);
			}
		}

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
