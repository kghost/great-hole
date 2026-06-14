#include "UtilConsole.hpp"

#include <boost/asio.hpp>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "Endpoint.hpp"

namespace gh {

class Input : public EndpointInput {
public:
  explicit Input(boost::asio::any_io_executor executor, decltype(STDERR_FILENO) f) : _S(executor, f) {}

  Omni::Fiber::Coroutine<ErrorCode> Read(Packet& p, Cancel& c) override {
    auto [err, bytes_transferred] =
        co_await _S.async_read_some(p.operator boost::asio::mutable_buffer(), Omni::Fiber::AsioUseFiber);
    if (err) {
      co_return err;
    }
    p._Length = bytes_transferred;
    co_return err;
  }

private:
  boost::asio::posix::stream_descriptor _S;
};

class Output : public EndpointOutput {
public:
  explicit Output(boost::asio::any_io_executor executor, decltype(STDERR_FILENO) f) : _S(executor, f) {}

  Omni::Fiber::Coroutine<ErrorCode> Write(Packet& p, Cancel& c) override {
    std::size_t sent = 0;
    while (sent < p._Length) {
      auto [err, bytes_transferred] =
          co_await _S.async_write_some(boost::asio::const_buffer{p} + sent, Omni::Fiber::AsioUseFiber);
      if (err) {
        co_return err;
      }
      sent += bytes_transferred;
    }
    co_return ErrorCode{};
  }

private:
  boost::asio::posix::stream_descriptor _S;
};

static std::weak_ptr<EndpointInput> _In;
static std::weak_ptr<EndpointOutput> _Out;
static std::weak_ptr<EndpointOutput> _Err;

std::shared_ptr<EndpointInput> GetCin(boost::asio::any_io_executor executor) {
  auto p = _In.lock();
  if (p) {
    return p;
  } else {
    auto o = std::make_shared<Input>(executor, STDIN_FILENO);
    _In = o;
    return o;
  }
}

std::shared_ptr<EndpointOutput> GetCout(boost::asio::any_io_executor executor) {
  auto p = _Out.lock();
  if (p) {
    return p;
  } else {
    auto o = std::make_shared<Output>(executor, STDOUT_FILENO);
    _Out = o;
    return o;
  }
}

std::shared_ptr<EndpointOutput> GetCerr(boost::asio::any_io_executor executor) {
  auto p = _Err.lock();
  if (p) {
    return p;
  } else {
    auto o = std::make_shared<Output>(executor, STDERR_FILENO);
    _Err = o;
    return o;
  }
}

} // namespace gh
