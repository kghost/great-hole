#include "pipeline.hpp"

#include <memory>

#include <boost/asio/buffer.hpp>
#include <boost/log/trivial.hpp>

#include "Event.hpp"
#include "GetCurrentFiber.hpp"
#include "endpoint.hpp"
#include "filter.hpp"

namespace gh {

Pipeline::Pipeline(std::shared_ptr<EndpointInput> in, const std::vector<std::shared_ptr<Filter>>& filters,
                   std::shared_ptr<EndpointOutput> out)
    : _In(in), _Out(out), _Filters(filters) {}

Omni::Fiber::Coroutine<void> Pipeline::Start(Omni::Fiber::Event<>& stopSignal) {
  BOOST_LOG_TRIVIAL(info) << "Pipeline(" << this << ") starting";

  ErrorCode ec = co_await _In->Start(stopSignal);
  if (ec) {
    BOOST_LOG_TRIVIAL(error) << "Pipeline(" << this << ") input Start error: " << ec.message();
    throw boost::system::system_error(ec, "Pipeline input start error");
  }
  ec = co_await _Out->Start(stopSignal);
  if (ec) {
    BOOST_LOG_TRIVIAL(error) << "Pipeline(" << this << ") output Start error: " << ec.message();
    throw boost::system::system_error(ec, "Pipeline output start error");
  }

  BOOST_LOG_TRIVIAL(info) << "Pipeline(" << this << ") started";

  auto& fiber = co_await Omni::Fiber::GetCurrentFiber();
  fiber.Spawn("pipe-work", [this]() mutable -> Omni::Fiber::Coroutine<void> {
    while (true) {
      auto [err, packet] = co_await _In->Read();
      if (err) {
        if (IsCritical(err)) {
          BOOST_LOG_TRIVIAL(error) << "Pipeline(" << this << ") read error: " << err.message();
          throw boost::system::system_error(err, "Pipeline read error");
        } else {
          BOOST_LOG_TRIVIAL(warning) << "Pipeline(" << this << ") read error (non-critical): " << err.message();
        }
      }
      for (auto& i : _Filters) {
        auto [err_pipe, packet2] = co_await i->Pipe(std::move(packet));
        if (err_pipe) {
          if (IsCritical(err_pipe)) {
            BOOST_LOG_TRIVIAL(error) << "Pipeline(" << this << ") filter error: " << err_pipe.message();
            throw boost::system::system_error(err_pipe, "Pipeline filter error");
          } else {
            BOOST_LOG_TRIVIAL(warning) << "Pipeline(" << this
                                       << ") filter error (non-critical): " << err_pipe.message();
          }
        }
        packet = std::move(packet2);
      }
      auto [err_write, bytes_transferred] = co_await _Out->Write(std::move(packet));
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

  co_await fiber.WaitAll();
  co_return;
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
