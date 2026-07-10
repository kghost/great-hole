#pragma once

#include <cstdint>
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
  explicit ServiceBase() = default;
  ~ServiceBase() override = default;

  ServiceBase(const ServiceBase&) = delete;
  auto operator=(const ServiceBase&) -> ServiceBase& = delete;
  ServiceBase(ServiceBase&&) = delete;
  auto operator=(ServiceBase&&) -> ServiceBase& = delete;

  auto Start() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto Stop() -> Omni::Fiber::Coroutine<ErrorCode> override;

  virtual auto GetName() const -> std::string = 0;

  enum class State : uint8_t { kNone, kPreStart, kStarting, kRunning, kStopping, kFinished, kError };
  auto GetState() const -> State { return _State; }

protected:
  struct Context {
    Cancel _Stop;
    Omni::Fiber::Event<ErrorCode> _StopError;
    std::shared_ptr<Omni::Fiber::Fiber> _Fiber;
  };

  State _State = State::kNone;
  std::optional<Context> _Service;

  virtual auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> = 0;
  virtual auto DoWork() -> Omni::Fiber::Coroutine<void>;
  virtual auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> = 0;
};

} // namespace gh
