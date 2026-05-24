#include "util-exec.hpp"

#include <csignal>

#include <boost/asio.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/detail/on_exit.hpp>
#include <boost/process/v1/env.hpp>
#include <boost/process/v1/environment.hpp>
#include <boost/process/v1/io.hpp>

namespace bp = boost::process::v1;

namespace gh {

class ExitErrorCategory : public ErrorCategory {
public:
  const char* name() const noexcept override { return "exit error"; }
  std::string message(int ev) const override { return ""; }
} kExitError;

class Exec::Proc : public std::enable_shared_from_this<Proc> {
public:
  explicit Proc(std::move_only_function<Event>&& handler) : _Handler(std::move(handler)) {}

  void Start(bp::child&& p) { _P = std::move(p); }

  void Kill(int signum) {
    if (_P.running()) {
      ::kill(_P.id(), signum);
    }
  }

  std::move_only_function<Event> _Handler;
  bp::child _P;
};

class Exec::Input : public EndpointSkipStart<EndpointOutput> {
public:
  Input(boost::asio::io_context& io_context, std::shared_ptr<boost::process::v1::async_pipe> pipe) : _Pipe(std::move(pipe)) {}

  void AsyncWrite(Packet&& p, std::move_only_function<WriteHandler>&& handler) override {
    boost::asio::async_write(*_Pipe, boost::asio::const_buffer{p.first}, std::move(handler));
  }

private:
  std::shared_ptr<boost::process::v1::async_pipe> _Pipe;
};

class Exec::Output : public EndpointSkipStart<EndpointInput> {
public:
  Output(boost::asio::io_context& io_context, std::shared_ptr<boost::process::v1::async_pipe> pipe) : _Pipe(std::move(pipe)) {}

  void AsyncRead(std::move_only_function<ReadHandler>&& handler) override {
    auto a = std::make_shared<std::array<uint8_t, 2048>>();
    auto p = Packet{Buffer(*a), a};
    auto buffer = boost::asio::mutable_buffer{p.first};
    _Pipe->async_read_some(buffer, [p{std::move(p)}, handler = std::move(handler)](
                                      const ErrorCode& ec, std::size_t bytes_transferred) mutable {
      if (!ec) {
        assert(bytes_transferred <= p.first.Capacity - p.first.Offset);
        p.first.Length = bytes_transferred;
      }
      handler(ec, std::move(p));
    });
  }

private:
  std::shared_ptr<boost::process::v1::async_pipe> _Pipe;
};

Exec::~Exec() { Kill(); }

std::shared_ptr<EndpointOutput> Exec::GetIn() {
  if (!_In) {
    _ChildIn = std::make_shared<boost::process::v1::async_pipe>(_IoContext);
    _In.reset(new Input(_IoContext, _ChildIn));
  }
  return _In;
}

std::shared_ptr<EndpointInput> Exec::GetOut() {
  if (!_Out) {
    _ChildOut = std::make_shared<boost::process::v1::async_pipe>(_IoContext);
    _Out.reset(new Output(_IoContext, _ChildOut));
  }
  return _Out;
}

std::shared_ptr<EndpointInput> Exec::GetErr() {
  if (!_Err) {
    _ChildErr = std::make_shared<boost::process::v1::async_pipe>(_IoContext);
    _Err.reset(new Output(_IoContext, _ChildErr));
  }
  return _Err;
}

void Exec::Run(std::move_only_function<Event>&& handler) {
  if (_P.lock()) {
    BOOST_LOG_TRIVIAL(error) << "proc " << this << " already running.";
    handler(ErrorCode(AppErrorCategory::kAlreadyStarted, kAppError));
  } else {
    try {
      auto env = boost::this_process::environment();
      bp::environment env_child = env;
      for (auto const& [k, v] : _Env) {
        env_child[k] = v;
      }

      auto i = std::make_shared<Proc>(std::move(handler));
      auto self = i;

      // Ensure all pipes are initialized so they have a consistent type for redirection expressions
      if (!_ChildIn) {
        _ChildIn = std::make_shared<boost::process::v1::async_pipe>(_IoContext);
      }
      if (!_ChildOut) {
        _ChildOut = std::make_shared<boost::process::v1::async_pipe>(_IoContext);
      }
      if (!_ChildErr) {
        _ChildErr = std::make_shared<boost::process::v1::async_pipe>(_IoContext);
      }

      auto c = bp::child(_Prog, _Args, env_child,
                         bp::std_in < *_ChildIn,
                         bp::std_out > *_ChildOut,
                         bp::std_err > *_ChildErr,
                         _IoContext,
                         bp::on_exit([self](int exit_code, const std::error_code& ec) {
                           ErrorCode boost_ec(ec);
                           if (!ec) {
                             BOOST_LOG_TRIVIAL(info) << "process exited: " << exit_code;
                             if (exit_code == 0) {
                               self->_Handler(ErrorCode());
                             } else {
                               self->_Handler(ErrorCode(exit_code, kExitError));
                             }
                           } else if (boost_ec != boost::asio::error::operation_aborted) {
                             BOOST_LOG_TRIVIAL(error) << "process wait error: " << ec.message();
                             self->_Handler(boost_ec);
                           }
                         }));

      i->Start(std::move(c));
      _P = i;

      _ChildIn.reset();
      _ChildOut.reset();
      _ChildErr.reset();
    } catch (const boost::system::system_error& e) {
      BOOST_LOG_TRIVIAL(error) << "proc " << this << " execute failed: " << e.what();
      handler(ErrorCode{AppErrorCategory::kForkExecError, kAppError});
    }
  }
}

void Exec::Kill() {
  if (auto s = _P.lock()) {
    s->Kill(SIGTERM);
  }
}

} // namespace gh
