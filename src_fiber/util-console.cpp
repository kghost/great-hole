#include "util-console.hpp"

#include <boost/asio.hpp>

#include "Asio.hpp"
#include "endpoint.hpp"

namespace gh {

class Input : public EndpointSkipStart<EndpointInput> {
public:
  explicit Input(boost::asio::io_context& io_context, decltype(STDERR_FILENO) f) : _S(io_context, f) {}

  Omni::Fiber::Coroutine<std::tuple<ErrorCode, Packet>> Read() override {
    auto a = std::make_shared<std::array<uint8_t, 2048>>();
    auto p = Packet{Buffer(*a), a};
    auto buffer = boost::asio::mutable_buffer{p.first};
    auto [err, bytes_transferred] = co_await _S.async_read_some(buffer, Omni::Fiber::AsioUseFiber);
    if (err) {
      throw boost::system::system_error(err);
    }
    assert(bytes_transferred <= p.first.Capacity - p.first.Offset);
    p.first.Length = bytes_transferred;
    co_return std::make_tuple(err, std::move(p));
  }

private:
  boost::asio::posix::stream_descriptor _S;
};

class Output : public EndpointSkipStart<EndpointOutput> {
public:
  explicit Output(boost::asio::io_context& io_context, decltype(STDERR_FILENO) f) : _S(io_context, f) {}

  Omni::Fiber::Coroutine<std::tuple<ErrorCode, std::size_t>> Write(Packet&& p) override {
    auto [err, bytes_transferred] =
        co_await _S.async_write_some(boost::asio::const_buffer{p.first}, Omni::Fiber::AsioUseFiber);
    if (err) {
      throw boost::system::system_error(err);
    }
    co_return std::make_tuple(err, bytes_transferred);
  }

private:
  boost::asio::posix::stream_descriptor _S;
};

static std::weak_ptr<EndpointInput> _In;
static std::weak_ptr<EndpointOutput> _Out;
static std::weak_ptr<EndpointOutput> _Err;

std::shared_ptr<EndpointInput> GetCin(boost::asio::io_context& io_context) {
  auto p = _In.lock();
  if (p) {
    return p;
  } else {
    auto o = std::make_shared<Input>(io_context, STDIN_FILENO);
    _In = o;
    return o;
  }
}

std::shared_ptr<EndpointOutput> GetCout(boost::asio::io_context& io_context) {
  auto p = _Out.lock();
  if (p) {
    return p;
  } else {
    auto o = std::make_shared<Output>(io_context, STDOUT_FILENO);
    _Out = o;
    return o;
  }
}

std::shared_ptr<EndpointOutput> GetCerr(boost::asio::io_context& io_context) {
  auto p = _Err.lock();
  if (p) {
    return p;
  } else {
    auto o = std::make_shared<Output>(io_context, STDERR_FILENO);
    _Err = o;
    return o;
  }
}

} // namespace gh
