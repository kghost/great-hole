#pragma once

#include <memory>

#include <boost/log/trivial.hpp>

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "Event.hpp"
#include "error-code.hpp"

namespace gh {

class Service : public std::enable_shared_from_this<Service> {
public:
  virtual ~Service() = default;

  virtual Omni::Fiber::Coroutine<ErrorCode> Start() = 0;
  virtual Omni::Fiber::Coroutine<ErrorCode> Stop() = 0;

  bool IsStopped() const { return _Stop.IsTriggered(); }

  void AddPipeline() {
    _PipelineCount++;
    BOOST_LOG_TRIVIAL(debug) << "Service(" << this << ") AddPipeline count: " << _PipelineCount;
  }

  void RemovePipeline() {
    _PipelineCount--;
    BOOST_LOG_TRIVIAL(debug) << "Service(" << this << ") RemovePipeline count: " << _PipelineCount;
    if (_PipelineCount == 0) {
      _GracefulExitEvent.Fire();
    }
  }

protected:
  Cancel _Stop;
  Omni::Fiber::Event<> _GracefulExitEvent;
  int _PipelineCount = 0;
};

} // namespace gh
