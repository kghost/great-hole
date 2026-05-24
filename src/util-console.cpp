#include "util-console.hpp"

#include <boost/asio.hpp>

#include "endpoint.hpp"

namespace gh {

class Input : public EndpointSkipStart<EndpointInput> {
public:
  explicit Input(boost::asio::io_context& io_context, decltype(STDERR_FILENO) f) : _S(io_context, f) {}

  void AsyncRead(std::move_only_function<ReadHandler>&& handler) override {
    auto a = std::make_shared<std::array<uint8_t, 2048>>();
    auto p = Packet{Buffer(*a), a};
    auto buffer = boost::asio::mutable_buffer{p.first};
    _S.async_read_some(buffer, [p{std::move(p)}, handler = std::move(handler)](const ErrorCode& ec,
                                                                               std::size_t bytes_transferred) mutable {
      if (!ec) {
        assert(bytes_transferred <= p.first.Capacity - p.first.Offset);
        p.first.Length = bytes_transferred;
      }
      handler(ec, std::move(p));
    });
  }

private:
  boost::asio::posix::stream_descriptor _S;
};

class Output : public EndpointSkipStart<EndpointOutput> {
public:
  explicit Output(boost::asio::io_context& io_context, decltype(STDERR_FILENO) f) : _S(io_context, f) {}

  void AsyncWrite(Packet&& p, std::move_only_function<WriteHandler>&& handler) override {
    boost::asio::async_write(_S, boost::asio::const_buffer{p.first}, std::move(handler));
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
