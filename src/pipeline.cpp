#include "pipeline.hpp"

#include <memory>

#include <boost/asio/buffer.hpp>
#include <boost/log/trivial.hpp>

#include "endpoint.hpp"
#include "filter.hpp"
#include "scoped_flag.hpp"

namespace gh {

Pipeline::Pipeline(std::shared_ptr<EndpointInput> in, const std::vector<std::shared_ptr<Filter>>& filters,
                   std::shared_ptr<EndpointOutput> out)
    : _In(in), _Out(out), _Filters(filters), _Fc(this) {}

void Pipeline::Start() {
  if (_State != kNone) {
    BOOST_LOG_TRIVIAL(error) << "Pipeline(" << this << ") already started: " << _State;
    return;
  }
  BOOST_LOG_TRIVIAL(info) << "Pipeline(" << this << ") starting";
  _State = kStarting;
  std::shared_ptr<char> result(new char, [me = shared_from_this()](char* r) {
    std::unique_ptr<char> _u(r);
    if (me->_State == kStarting) {
      BOOST_LOG_TRIVIAL(info) << "Pipeline " << &*me << " started";
      me->_State = kRunning;
      me->ScheduleRead();
    }
  });
  _In->AsyncStart([result, me = shared_from_this()](const ErrorCode& ec) {
    if (ec) {
      BOOST_LOG_TRIVIAL(error) << "Pipeline(" << &*me << ") input Start error: " << ec.message();
      me->_State = kError;
    }
  });
  _Out->AsyncStart([result, me = shared_from_this()](const ErrorCode& ec) {
    if (ec) {
      BOOST_LOG_TRIVIAL(error) << "Pipeline(" << &*me << ") output Start error: " << ec.message();
      me->_State = kError;
    }
  });
}

void Pipeline::Stop() {
  if (_State != kRunning && _State != kPaused) {
    BOOST_LOG_TRIVIAL(error) << "Pipeline(" << this << ") is not running: " << _State;
    return;
  }
  _State = kStopped;
}

void Pipeline::Pause() {
  if (_State != kRunning) {
    BOOST_LOG_TRIVIAL(error) << "Pipeline(" << this << ") is not running: " << _State;
    return;
  }
  _State = kPaused;
}

void Pipeline::Resume() {
  if (_State != kPaused) {
    BOOST_LOG_TRIVIAL(error) << "Pipeline(" << this << ") is not paused: " << _State;
    return;
  }
  _State = kRunning;
  ScheduleRead();
}

void Pipeline::Process(ScopedFlag&& write, Packet&& p) {
  for (auto& i : _Filters) {
    p = i->Pipe(std::move(p));
  }
  ScheduleWrite(std::move(write), std::move(p));
}

void Pipeline::ProcessQueue(ScopedFlag&& write) {
  if (!_Buffers.empty()) {
    auto next = std::move(_Buffers.front());
    _Buffers.pop();
    Process(std::move(write), std::move(next));
  }
}

void Pipeline::ScheduleRead() {
  if (_State == kRunning && !_ReadPending) {
    ScheduleRead(ScopedFlag(_ReadPending));
  }
}

void Pipeline::ScheduleRead(ScopedFlag&& read) {
  if (_State == kRunning) {
    _In->AsyncRead([this, me = shared_from_this(), read{std::move(read)}](const ErrorCode& ec, Packet&& p) mutable {
      // _Fc.AfterRead();
      if (!ec) {
        if (!_WritePending) {
          Process(ScopedFlag(_WritePending), std::move(p));
        } else {
          _Buffers.push(std::move(p));
        }
      } else {
        if (me->IsCritical(ec)) {
          BOOST_LOG_TRIVIAL(error) << "Pipeline(" << this << ") read error: " << ec.message();
          Stop();
        } else {
          BOOST_LOG_TRIVIAL(warning) << "Pipeline(" << this << ") read error (non-critical): " << ec.message();
        }
      }
      ScheduleRead(std::move(read));
    });
  }
}

void Pipeline::ScheduleWrite(ScopedFlag&& write, Packet&& p) {
  if (_State == kRunning || _State == kPaused) {
    _Out->AsyncWrite(std::move(p), [me = shared_from_this(), write{std::move(write)}](
                                       const ErrorCode& ec, std::size_t bytes_transferred) mutable {
      // me->_Fc.AfterWrite();
      if (ec) {
        if (me->IsCritical(ec)) {
          BOOST_LOG_TRIVIAL(error) << "Pipeline(" << &*me << ") write error: " << ec.message();
          me->Stop();
        } else {
          BOOST_LOG_TRIVIAL(warning) << "Pipeline(" << &*me << ") write error (non-critical): " << ec.message();
        }
      }
      me->ProcessQueue(std::move(write));
    });
  }
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
