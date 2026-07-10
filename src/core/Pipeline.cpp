#include "Pipeline.hpp"

#include <memory>

#include "PipielineUsageCounter.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/log/trivial.hpp>

#include "Endpoint.hpp"
#include "ErrorCode.hpp"
#include "Filter.hpp"
#include "GetCurrentOmniFiber.hpp"

namespace gh {

Pipeline::Pipeline(std::shared_ptr<Endpoint> ep1, const std::vector<std::shared_ptr<Filter>>& filters,
                   std::shared_ptr<Endpoint> ep2)
    : _Ep1(ep1), _Ep2(ep2), _Filters(filters) {}

auto Pipeline::Start() -> Omni::Fiber::Coroutine<ErrorCode> {
  auto me = std::static_pointer_cast<Pipeline>(shared_from_this());
  auto& fiber = co_await Omni::Fiber::GetCurrentOmniFiber();

  _Fiber1 = fiber.Spawn(GetNameWithDirection(Direction::Forward), [this, me]() -> Omni::Fiber::Coroutine<void> {
    co_await RunDirection(_Ep1, _Ep2, Direction::Forward);
  });

  _Fiber2 = fiber.Spawn(GetNameWithDirection(Direction::Backward), [this, me]() -> Omni::Fiber::Coroutine<void> {
    co_await RunDirection(_Ep2, _Ep1, Direction::Backward);
  });

  co_return ErrorCode{};
}

auto Pipeline::RunDirection(std::shared_ptr<Endpoint> in, std::shared_ptr<Endpoint> out,
                                                    Pipeline::Direction direction) -> Omni::Fiber::Coroutine<void> {
  BOOST_LOG_TRIVIAL(info) << std::format("{} direction started", GetNameWithDirection(direction));
  struct ActivePipelineGuard {
    PipielineUsageCounter& In;
    PipielineUsageCounter& Out;
    ActivePipelineGuard(Endpoint& in, Endpoint& out)
        : In(in.GetPipielineUsageCounter()), Out(out.GetPipielineUsageCounter()) {
      In.AddPipeline();
      Out.AddPipeline();
    }
    ~ActivePipelineGuard() {
      In.RemovePipeline();
      Out.RemovePipeline();
    }
  } guard(*in, *out);

  while (!_Stop.IsTriggered()) {
    Packet p;
    auto errRead = co_await in->Read(p, _Stop);
    if (errRead) {
      if (errRead == ErrorCode{AppErrorCategory::kOperationAborted, kAppError}) {
        BOOST_LOG_TRIVIAL(info) << std::format("{} read cancelled detected", GetNameWithDirection(direction));
        continue;
      } else if (errRead == ErrorCode{AppErrorCategory::kEndOfStream, kAppError}) {
        BOOST_LOG_TRIVIAL(info) << std::format("{} EoF detected", GetNameWithDirection(direction));
        break;
      } else if (IsCritical(errRead)) {
        BOOST_LOG_TRIVIAL(error) << std::format("{} read error: {}", GetNameWithDirection(direction),
                                                errRead.message());
        throw boost::system::system_error(errRead, "Pipeline read error");
      } else {
        BOOST_LOG_TRIVIAL(warning) << std::format("{} read error (non-critical): {}", GetNameWithDirection(direction),
                                                  errRead.message());
      }
    }
    for (std::size_t idx = 0; idx < _Filters.size(); ++idx) {
      auto& i = _Filters[direction == Direction::Forward ? idx : (_Filters.size() - 1 - idx)];
      auto errPipe = co_await i->Pipe(p, _Stop);
      if (errPipe) {
        if (errPipe == ErrorCode{AppErrorCategory::kOperationAborted, kAppError}) {
          BOOST_LOG_TRIVIAL(info) << std::format("{} filter cancelled detected", GetNameWithDirection(direction));
          continue;
        } else if (IsCritical(errPipe)) {
          BOOST_LOG_TRIVIAL(error) << std::format("{} filter error: {}", GetNameWithDirection(direction),
                                                  errPipe.message());
          throw boost::system::system_error(errPipe, "Pipeline filter error");
        } else {
          BOOST_LOG_TRIVIAL(warning) << std::format("{} filter error (non-critical): {}",
                                                    GetNameWithDirection(direction), errPipe.message());
        }
      }
    }
    auto errWrite = co_await out->Write(p, _Stop);
    if (errWrite) {
      if (errWrite == ErrorCode{AppErrorCategory::kOperationAborted, kAppError}) {
        BOOST_LOG_TRIVIAL(info) << std::format("{} write cancelled detected", GetNameWithDirection(direction));
        continue;
      } else if (IsCritical(errWrite)) {
        BOOST_LOG_TRIVIAL(error) << std::format("{} write error: {}", GetNameWithDirection(direction),
                                                errWrite.message());
        throw boost::system::system_error(errWrite, "Pipeline write error");
      } else {
        BOOST_LOG_TRIVIAL(warning) << std::format("{} write error (non-critical): {}", GetNameWithDirection(direction),
                                                  errWrite.message());
      }
    }

    if (direction == Direction::Forward) {
      _TrafficStats.OnForward(p._Length);
    } else {
      _TrafficStats.OnBackword(p._Length);
    }
  }
  BOOST_LOG_TRIVIAL(info) << std::format("{} direction exited", GetNameWithDirection(direction));
  co_return;
}

auto Pipeline::Stop() -> Omni::Fiber::Coroutine<ErrorCode> {
  _Stop.Trigger();
  auto& current = co_await Omni::Fiber::GetCurrentOmniFiber();
  if (_Fiber1) {
    co_await current.Join(_Fiber1);
    _Fiber1.reset();
  }
  if (_Fiber2) {
    co_await current.Join(_Fiber2);
    _Fiber2.reset();
  }
  co_return ErrorCode{};
}

auto Pipeline::IsCritical(const ErrorCode& ec) -> bool {
  if (!ec) {
    return false;
  } else if (ec.category() == boost::system::system_category()) {
    switch (ec.value()) {
    case boost::system::errc::invalid_argument:
    case boost::system::errc::io_error:
    case boost::system::errc::connection_refused:
    case boost::system::errc::network_unreachable:
    case boost::system::errc::host_unreachable:
    case boost::system::errc::operation_canceled:
      return false;
    default:
      return true;
    }
  } else if (ec.category() == kAppError) {
    return true;
  } else if (ec.category() == kAppMinorError) {
    return false;
  } else {
    return true;
  }
}

} // namespace gh
