#include "util-exec.hpp"

#include <signal.h>

#include <boost/asio.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>

namespace bp2 = boost::process::v2;

namespace gh {

class ExitErrorCategory : public ErrorCategory {
public:
  const char* name() const noexcept override { return "exit error"; }
  std::string message(int ev) const override { return ""; }
} kExitError;

class Exec::Proc : public std::enable_shared_from_this<Proc> {
public:
  Proc(std::move_only_function<Event>&& handler, bp2::process&& p) : _Handler(std::move(handler)), _P(std::move(p)) {}

  void StartWait() {
    auto self = shared_from_this();
    _P.async_wait([self](const ErrorCode& ec, int exit_code) {
      if (!ec) {
        BOOST_LOG_TRIVIAL(info) << "process exited: " << exit_code;
        if (exit_code == 0) {
          self->_Handler(ErrorCode());
        } else {
          self->_Handler(ErrorCode(exit_code, kExitError));
        }
      } else if (ec != boost::asio::error::operation_aborted) {
        BOOST_LOG_TRIVIAL(error) << "process wait error: " << ec;
        self->_Handler(ec);
      }
    });
  }

  void Kill(int signum) { ::kill(_P.id(), signum); }

private:
  std::move_only_function<Event> _Handler;
  bp2::process _P;
};

class Exec::Input : public EndpointSkipStart<EndpointOutput> {
public:
  Input(boost::asio::io_context& io_context, boost::asio::writable_pipe pipe) : _Pipe(std::move(pipe)) {}

  void AsyncWrite(Packet&& p, std::move_only_function<WriteHandler>&& handler) override {
    boost::asio::async_write(_Pipe, boost::asio::const_buffer{p.first}, std::move(handler));
  }

private:
  boost::asio::writable_pipe _Pipe;
};

class Exec::Output : public EndpointSkipStart<EndpointInput> {
public:
  Output(boost::asio::io_context& io_context, boost::asio::readable_pipe pipe) : _Pipe(std::move(pipe)) {}

  void AsyncRead(std::move_only_function<ReadHandler>&& handler) override {
    auto a = std::make_shared<std::array<uint8_t, 2048>>();
    auto p = Packet{Buffer(*a), a};
    auto buffer = boost::asio::mutable_buffer{p.first};
    _Pipe.async_read_some(buffer, [p{std::move(p)}, handler = std::move(handler)](
                                     const ErrorCode& ec, std::size_t bytes_transferred) mutable {
      if (!ec) {
        assert(bytes_transferred <= p.first.Capacity - p.first.Offset);
        p.first.Length = bytes_transferred;
      }
      handler(ec, std::move(p));
    });
  }

private:
  boost::asio::readable_pipe _Pipe;
};

Exec::~Exec() { Kill(); }

std::shared_ptr<EndpointOutput> Exec::GetIn() {
  if (!_In) {
    boost::asio::readable_pipe read_end(_IoContext);
    boost::asio::writable_pipe write_end(_IoContext);
    boost::asio::connect_pipe(read_end, write_end);
    _In.reset(new Input(_IoContext, std::move(write_end)));
    _ChildIn = std::move(read_end);
  }
  return _In;
}

std::shared_ptr<EndpointInput> Exec::GetOut() {
  if (!_Out) {
    boost::asio::readable_pipe read_end(_IoContext);
    boost::asio::writable_pipe write_end(_IoContext);
    boost::asio::connect_pipe(read_end, write_end);
    _Out.reset(new Output(_IoContext, std::move(read_end)));
    _ChildOut = std::move(write_end);
  }
  return _Out;
}

std::shared_ptr<EndpointInput> Exec::GetErr() {
  if (!_Err) {
    boost::asio::readable_pipe read_end(_IoContext);
    boost::asio::writable_pipe write_end(_IoContext);
    boost::asio::connect_pipe(read_end, write_end);
    _Err.reset(new Output(_IoContext, std::move(read_end)));
    _ChildErr = std::move(write_end);
  }
  return _Err;
}

void Exec::Run(std::move_only_function<Event>&& handler) {
  if (_P.lock()) {
    BOOST_LOG_TRIVIAL(error) << "proc " << this << " already running.";
    handler(ErrorCode(AppErrorCategory::kAlreadyStarted, kAppError));
  } else {
    try {
      std::vector<std::string> e;
      for (auto const& [k, v] : _Env) {
        e.push_back((boost::format("%1%=%2%") % k % v).str());
      }

      bp2::process_stdio stdio;
      if (_ChildIn) {
        stdio.in = *_ChildIn;
      }
      if (_ChildOut) {
        stdio.out = *_ChildOut;
      }
      if (_ChildErr) {
        stdio.err = *_ChildErr;
      }

      auto i = std::make_shared<Proc>(std::move(handler),
                                      bp2::process(_IoContext, _Prog, _Args, stdio, bp2::process_environment{e}));
      i->StartWait();
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
