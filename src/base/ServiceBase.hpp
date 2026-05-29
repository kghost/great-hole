#pragma once

#include <memory>

#include <boost/log/trivial.hpp>

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "Event.hpp"
#include "Service.hpp"

namespace gh {

class ServiceBase : public std::enable_shared_from_this<ServiceBase>, virtual public Service {
public:
  virtual ~ServiceBase() = default;

  Omni::Fiber::Coroutine<ErrorCode> Start() override;
  Omni::Fiber::Coroutine<ErrorCode> Stop() override;

  virtual std::string GetName() const = 0;
  bool IsStopped() const { return _Stop.IsTriggered(); }

protected:
  Cancel _Stop;
  Omni::Fiber::Event<ErrorCode> _StopError;

  virtual Omni::Fiber::Coroutine<ErrorCode> DoStart() = 0;
  virtual Omni::Fiber::Coroutine<void> DoWork();
  virtual Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() = 0;
};

} // namespace gh
