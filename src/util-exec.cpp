#include "util-exec.hpp"

#include <signal.h>

#include <boost/asio.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>

namespace bp2 = boost::process::v2;

class exit_error_category : public gh::error_category {
public:
  virtual const char* name() const noexcept { return "exit error"; }
  virtual std::string message(int ev) const { return ""; }
} exit_error;

class exec::proc : public std::enable_shared_from_this<proc> {
public:
  proc(std::move_only_function<event>&& handler, bp2::process&& p) : handler(std::move(handler)), p(std::move(p)) {}

  void start_wait() {
    auto self = shared_from_this();
    p.async_wait([self](const gh::error_code& ec, int exit_code) {
      if (!ec) {
        BOOST_LOG_TRIVIAL(info) << "process exited: " << exit_code;
        if (exit_code == 0) {
          self->handler(gh::error_code());
        } else {
          self->handler(gh::error_code(exit_code, exit_error));
        }
      } else if (ec != boost::asio::error::operation_aborted) {
        BOOST_LOG_TRIVIAL(error) << "process wait error: " << ec;
        self->handler(ec);
      }
    });
  }

  void kill(int signum) { ::kill(p.id(), signum); }

private:
  std::move_only_function<event> handler;
  bp2::process p;
};

class exec::input : public endpoint_skip_start<endpoint_output> {
public:
  input(boost::asio::io_context& io_context, boost::asio::writable_pipe pipe) : pipe(std::move(pipe)) {}

  void async_write(packet&& p, std::move_only_function<write_handler>&& handler) override {
    boost::asio::async_write(pipe, boost::asio::const_buffer{p.first}, std::move(handler));
  }

private:
  boost::asio::writable_pipe pipe;
};

class exec::output : public endpoint_skip_start<endpoint_input> {
public:
  output(boost::asio::io_context& io_context, boost::asio::readable_pipe pipe) : pipe(std::move(pipe)) {}

  virtual void async_read(std::move_only_function<read_handler>&& handler) {
    auto a = std::make_shared<std::array<uint8_t, 2048>>();
    auto p = packet{buffer(*a), a};
    auto buffer = boost::asio::mutable_buffer{p.first};
    pipe.async_read_some(buffer, [p{std::move(p)}, handler = std::move(handler)](
                                     const gh::error_code& ec, std::size_t bytes_transferred) mutable {
      if (!ec) {
        assert(bytes_transferred <= p.first.capacity - p.first.offset);
        p.first.length = bytes_transferred;
      }
      handler(ec, std::move(p));
    });
  }

private:
  boost::asio::readable_pipe pipe;
};

exec::~exec() { kill(); }

std::shared_ptr<endpoint_output> exec::get_in() {
  if (!in) {
    boost::asio::readable_pipe read_end(io_context);
    boost::asio::writable_pipe write_end(io_context);
    boost::asio::connect_pipe(read_end, write_end);
    in.reset(new input(io_context, std::move(write_end)));
    child_in = std::move(read_end);
  }
  return in;
}

std::shared_ptr<endpoint_input> exec::get_out() {
  if (!out) {
    boost::asio::readable_pipe read_end(io_context);
    boost::asio::writable_pipe write_end(io_context);
    boost::asio::connect_pipe(read_end, write_end);
    out.reset(new output(io_context, std::move(read_end)));
    child_out = std::move(write_end);
  }
  return out;
}

std::shared_ptr<endpoint_input> exec::get_err() {
  if (!err) {
    boost::asio::readable_pipe read_end(io_context);
    boost::asio::writable_pipe write_end(io_context);
    boost::asio::connect_pipe(read_end, write_end);
    err.reset(new output(io_context, std::move(read_end)));
    child_err = std::move(write_end);
  }
  return err;
}

void exec::run(std::move_only_function<event>&& handler) {
  if (p.lock()) {
    BOOST_LOG_TRIVIAL(error) << "proc " << this << " already running.";
    handler(gh::error_code(app_error_category::already_started, app_error));
  } else {
    try {
      std::vector<std::string> e;
      for (auto const& [k, v] : env) {
        e.push_back((boost::format("%1%=%2%") % k % v).str());
      }

      bp2::process_stdio stdio;
      if (child_in) {
        stdio.in = *child_in;
      }
      if (child_out) {
        stdio.out = *child_out;
      }
      if (child_err) {
        stdio.err = *child_err;
      }

      auto i = std::make_shared<proc>(std::move(handler),
                                      bp2::process(io_context, prog, args, stdio, bp2::process_environment{e}));
      i->start_wait();
      p = i;

      child_in.reset();
      child_out.reset();
      child_err.reset();
    } catch (const boost::system::system_error& e) {
      BOOST_LOG_TRIVIAL(error) << "proc " << this << " execute failed: " << e.what();
      handler(gh::error_code{app_error_category::fork_exec_error, app_error});
    }
  }
}

void exec::kill() {
  if (auto s = p.lock()) {
    s->kill(SIGTERM);
  }
}
