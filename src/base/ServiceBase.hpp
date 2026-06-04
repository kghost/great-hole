#pragma once

#include <memory>

#include <boost/log/trivial.hpp>
#include <optional>

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

  enum class State { kNone, kPreStart, kStarting, kRunning, kStopping, kFinished, kError };
  State GetState() const { return _State; }

  // This must be call in the same fiber where Start() is called. After calling this function, the state will return to
  // kNone, and the service can be restarted.
  Omni::Fiber::Coroutine<void> WaitService();

protected:
  struct Context {
    Cancel _Stop;
    Omni::Fiber::Event<ErrorCode> _StopError;
    std::shared_ptr<Omni::Fiber::Fiber> _Fiber;
  };
  State _State = State::kNone;
  std::optional<Context> _Service;

  virtual Omni::Fiber::Coroutine<ErrorCode> DoStart() = 0;
  virtual Omni::Fiber::Coroutine<void> DoWork();
  virtual Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() = 0;
};

} // namespace gh
