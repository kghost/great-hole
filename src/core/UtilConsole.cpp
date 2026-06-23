#include "UtilConsole.hpp"

#include <boost/asio.hpp>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "Endpoint.hpp"

namespace gh {

#if _WIN32
using FileHandleType = boost::asio::windows::stream_handle;
#else
using FileHandleType = boost::asio::posix::stream_descriptor;
#endif

class Input : public EndpointInput {
public:
  explicit Input(boost::asio::any_io_executor executor, FileHandleType::native_handle_type f) : _S(executor, f) {}

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
  FileHandleType _S;
};

class Output : public EndpointOutput {
public:
  explicit Output(boost::asio::any_io_executor executor, FileHandleType::native_handle_type f) : _S(executor, f) {}

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
  FileHandleType _S;
};

static std::weak_ptr<EndpointInput> _In;
static std::weak_ptr<EndpointOutput> _Out;
static std::weak_ptr<EndpointOutput> _Err;

std::shared_ptr<EndpointInput> GetCin(boost::asio::any_io_executor executor) {
  auto p = _In.lock();
  if (p) {
    return p;
  } else {
#if _WIN32
    auto o = std::make_shared<Input>(executor, ::GetStdHandle(STD_INPUT_HANDLE));
#else
    auto o = std::make_shared<Input>(executor, _fileno(stdin));
#endif
    _In = o;
    return o;
  }
}

std::shared_ptr<EndpointOutput> GetCout(boost::asio::any_io_executor executor) {
  auto p = _Out.lock();
  if (p) {
    return p;
  } else {
#if _WIN32
    auto o = std::make_shared<Output>(executor, ::GetStdHandle(STD_OUTPUT_HANDLE));
#else
    auto o = std::make_shared<Output>(executor, _fileno(stdout));
#endif
    _Out = o;
    return o;
  }
}

std::shared_ptr<EndpointOutput> GetCerr(boost::asio::any_io_executor executor) {
  auto p = _Err.lock();
  if (p) {
    return p;
  } else {
#if _WIN32
    auto o = std::make_shared<Output>(executor, ::GetStdHandle(STD_ERROR_HANDLE));
#else
    auto o = std::make_shared<Output>(executor, _fileno(stderr));
#endif
    _Err = o;
    return o;
  }
}

} // namespace gh
