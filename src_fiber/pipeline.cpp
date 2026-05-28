#include "pipeline.hpp"

#include <memory>

#include <boost/asio/buffer.hpp>
#include <boost/log/trivial.hpp>

#include "Event.hpp"
#include "GetCurrentFiber.hpp"
#include "endpoint.hpp"
#include "error-code.hpp"
#include "filter.hpp"

namespace gh {

Pipeline::Pipeline(std::shared_ptr<EndpointInput> in, const std::vector<std::shared_ptr<Filter>>& filters,
                   std::shared_ptr<EndpointOutput> out)
    : _In(in), _Out(out), _Filters(filters) {}

Omni::Fiber::Coroutine<ErrorCode> Pipeline::Start(Omni::Fiber::Event<>& stopSignal) {
  BOOST_LOG_TRIVIAL(info) << "Pipeline(" << this << ") started";

  auto& fiber = co_await Omni::Fiber::GetCurrentFiber();
  fiber.Spawn(std::format("Pipeline:{:p}", static_cast<void*>(this)), [this]() mutable -> Omni::Fiber::Coroutine<void> {
    while (true) {
      Packet p;
      auto err_read = co_await _In->Read(p);
      if (err_read) {
        if (IsCritical(err_read)) {
          BOOST_LOG_TRIVIAL(error) << "Pipeline(" << this << ") read error: " << err_read.message();
          throw boost::system::system_error(err_read, "Pipeline read error");
        } else {
          BOOST_LOG_TRIVIAL(warning) << "Pipeline(" << this << ") read error (non-critical): " << err_read.message();
        }
      }
      for (auto& i : _Filters) {
        auto err_pipe = co_await i->Pipe(p);
        if (err_pipe) {
          if (IsCritical(err_pipe)) {
            BOOST_LOG_TRIVIAL(error) << "Pipeline(" << this << ") filter error: " << err_pipe.message();
            throw boost::system::system_error(err_pipe, "Pipeline filter error");
          } else {
            BOOST_LOG_TRIVIAL(warning) << "Pipeline(" << this
                                       << ") filter error (non-critical): " << err_pipe.message();
          }
        }
      }
      auto err_write = co_await _Out->Write(p);
      if (err_write) {
        if (IsCritical(err_write)) {
          BOOST_LOG_TRIVIAL(error) << "Pipeline(" << this << ") write error: " << err_write.message();
          throw boost::system::system_error(err_write, "Pipeline write error");
        } else {
          BOOST_LOG_TRIVIAL(warning) << "Pipeline(" << this << ") write error (non-critical): " << err_write.message();
        }
      }
    }
    co_return;
  });

  co_return ErrorCode{};
}

bool Pipeline::IsCritical(const ErrorCode& ec) {
  if (ec.category() == boost::system::system_category()) {
    switch (ec.value()) {
    case boost::system::errc::invalid_argument:
    case boost::system::errc::io_error:
    case boost::system::errc::connection_refused:
    case boost::system::errc::network_unreachable:
    case boost::system::errc::host_unreachable:
      return false;
    default:
      return true;
    }
  } else if (ec.category() == kAppError) {
    switch (ec.value()) {
    case AppErrorCategory::kInvalidPacketSession:
    case AppErrorCategory::kInvalidPacketSize:
      return false;
    default:
      return true;
    }
  } else {
    return true;
  }
}

} // namespace gh
