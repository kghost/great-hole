#include "config.h"

#include "util-exec.hpp"

#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>

#include <algorithm>
#include <functional>

#include <boost/format.hpp>
#include <boost/asio.hpp>
#include <boost/log/trivial.hpp>
#include <boost/process.hpp>
#include <boost/process/mitigate.hpp>
#include <boost/iostreams/device/file_descriptor.hpp>

class exit_error_category : public gh::error_category {
	public:
		virtual const char * name() const noexcept { return "exit error"; }
		virtual std::string message(int ev) const { return ""; }
} exit_error;

class exec::signal {
	public:
		void add_proc(std::shared_ptr<proc> p);

		void schedule_wait_signal(boost::asio::io_service &io_service) {
			if (!set) {
				set.reset(new boost::asio::signal_set(io_service, SIGCHLD));
				set->async_wait([this](const gh::error_code &ec, int signal_number) { sigchld_handler(ec, signal_number); });
			}
		}

	private:
		void sigchld_handler(const gh::error_code &ec, int signal_number);

		std::unique_ptr<boost::asio::signal_set> set;
		std::map<pid_t, std::shared_ptr<proc>> running_processes;
} exec::s;

class exec::proc : public std::enable_shared_from_this<proc> {
	public:
		proc(fu2::unique_function<event> &&handler, pid_t pid) : handler(std::move(handler)), pid(pid) {}

		void notify(int code) {
			if (code != 0) {
				handler(gh::error_code());
			} else {
				handler(gh::error_code(code, exit_error));
			}
		}
		void kill(int signum) { ::kill(pid, signum); }
	private:
		fu2::unique_function<event> handler;
		pid_t pid;
		friend class signal;
};

void exec::signal::add_proc(std::shared_ptr<proc> p) { running_processes[p->pid] = p; }

void exec::signal::sigchld_handler(const gh::error_code &ec, int signal_number) {
	if (!ec) {
		for (;;) {
			int status;
			pid_t pid = ::waitpid(-1, &status, WNOHANG);
			if (pid == 0) break;
			if (pid < 0) {
				gh::error_code ec;
				ec.assign(errno, gh::system_category());
				boost::asio::detail::throw_error(ec, "waitpid");
			}
			auto search = running_processes.find(pid);
			if (search != running_processes.end()) {
				auto &p = search->second;
				if (WIFEXITED(status)) {
					BOOST_LOG_TRIVIAL(info) << "sigchld_handler process(" << pid << ") exited: " << WEXITSTATUS(status);
					p->notify(WEXITSTATUS(status));
					running_processes.erase(search);
				} else if (WIFSIGNALED(status)) {
					BOOST_LOG_TRIVIAL(info) << "sigchld_handler process(" << pid << ") killed: " << WTERMSIG(status);
					p->notify(0x80 | WTERMSIG(status));
					running_processes.erase(search);
				} else if (WIFSTOPPED(status)) {
					BOOST_LOG_TRIVIAL(info) << "sigchld_handler process(" << pid << ") stopped: " << WSTOPSIG(status);
				} else if (WIFCONTINUED(status)) {
					BOOST_LOG_TRIVIAL(info) << "sigchld_handler process(" << pid << ") continued";
				}
			} else {
				BOOST_LOG_TRIVIAL(error) << "sigchld_handler unknown pid: " << pid;
			}
		}
		set->async_wait([this](const gh::error_code &ec, int signal_number) { sigchld_handler(ec, signal_number); });
	} else if (ec == boost::asio::error::operation_aborted) {
		for (auto& process : running_processes) {
			process.second->kill(SIGHUP);
		}
		running_processes.clear();
	} else {
		BOOST_LOG_TRIVIAL(error) << "exec signal handler error: " << ec;
		boost::asio::detail::throw_error(ec, "sigchld_handler");
	}
}

class exec::input : public endpoint_skip_start<endpoint_output> {
	public:
		input(boost::asio::io_service &io_service, decltype(boost::process::pipe::sink) sink) : pipe(io_service, sink) {}

		void async_write(packet && p, fu2::unique_function<write_handler> && handler) override {
			boost::asio::async_write(pipe, boost::asio::const_buffer{p.first}, std::move(handler));
		}
	private:
		boost::process::pipe_end pipe;
};

class exec::output : public endpoint_skip_start<endpoint_input> {
	public:
		output(boost::asio::io_service &io_service, decltype(boost::process::pipe::source) source) : pipe(io_service, source) {}

		virtual void async_read(fu2::unique_function<read_handler> &&handler) {
			auto a = std::make_shared<std::array<uint8_t, 2048>>();
			auto p = packet{buffer(*a), a};
			auto buffer = boost::asio::mutable_buffer{p.first};
			pipe.async_read_some(buffer, [p{std::move(p)}, handler = std::move(handler)](const gh::error_code& ec, std::size_t bytes_transferred) mutable {
				if (!ec) {
					assert(bytes_transferred <= p.first.capacity - p.first.offset);
					p.first.length = bytes_transferred;
				}
				handler(ec, std::move(p));
			});
		}
	private:
		boost::process::pipe_end pipe;
};

boost::iostreams::file_descriptor_source exec::null_stream_source(boost::process::null_device());
boost::iostreams::file_descriptor_sink exec::null_stream_sink(boost::process::null_device());

std::shared_ptr<endpoint_output> exec::get_in() {
	if (!in) {
		boost::process::pipe p = boost::process::create_pipe();
		in.reset(new input(io_service, p.sink));
		child_in = boost::iostreams::file_descriptor_source(p.source, boost::iostreams::close_handle);
	}
	return in;
}

std::shared_ptr<endpoint_input> exec::get_out() {
	if (!out) {
		boost::process::pipe p = boost::process::create_pipe();
		out.reset(new output(io_service, p.source));
		child_out = boost::iostreams::file_descriptor_sink(p.sink, boost::iostreams::close_handle);
	}
	return out;
}

std::shared_ptr<endpoint_input> exec::get_err() {
	if (!err) {
		boost::process::pipe p = boost::process::create_pipe();
		err.reset(new output(io_service, p.source));
		child_err = boost::iostreams::file_descriptor_sink(p.sink, boost::iostreams::close_handle);
	}
	return err;
}

void exec::run(fu2::unique_function<event> &&handler) {
	if (p.lock()) {
		BOOST_LOG_TRIVIAL(error) << "proc " << this << " already running.";
		handler(gh::error_code(app_error_category::already_started, app_error));
	} else {
		using namespace boost::process;
		using namespace boost::process::initializers;

		s.schedule_wait_signal(io_service);

		std::vector<std::string> e(env.size());
		boost::transform(env, e.begin(), [](const typename decltype(env)::value_type &v) -> std::string {
			return (boost::format("%1%=%2%") % v.first % v.second).str();
		});

		child c = execute(
			run_exe(prog),
			set_args(args),
			set_env(e),
			notify_io_service(io_service),
			bind_stdin(child_in),
			bind_stdout(child_out),
			bind_stderr(child_err));
		if (c.pid > 0) {
			auto i = std::make_shared<proc>(std::move(handler), c.pid);
			s.add_proc(i);
			p = i;
		} else {
			BOOST_LOG_TRIVIAL(error) << "proc " << this << " execute failed.";
			handler(gh::error_code{app_error_category::fork_exec_error, app_error});
		}
	}
}

void exec::kill() {
	if (auto s = p.lock()) s->kill(SIGTERM);
}
