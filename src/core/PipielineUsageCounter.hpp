#pragma once

#include <boost/log/trivial.hpp>

#include "Coroutine.hpp"
#include "Event.hpp"

namespace gh {

class PipielineUsageCounter {
public:
  void AddPipeline() {
    _PipelineCount++;
    BOOST_LOG_TRIVIAL(debug) << "PipielineUsageCounter(" << this << ") AddPipeline count: " << _PipelineCount;
  }

  void RemovePipeline() {
    _PipelineCount--;
    BOOST_LOG_TRIVIAL(debug) << "PipielineUsageCounter(" << this << ") RemovePipeline count: " << _PipelineCount;
    if (_PipelineCount == 0) {
      _AllPipelineStopped.Fire();
    }
  }

  auto WaitAll() -> Omni::Fiber::Coroutine<void> {
    while (_PipelineCount != 0) {
      co_await _AllPipelineStopped;
    }
    co_return;
  }

protected:
  int _PipelineCount = 0;
  Omni::Fiber::Event<void> _AllPipelineStopped;
};

} // namespace gh
