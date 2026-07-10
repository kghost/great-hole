#pragma once

#include <boost/asio.hpp>
#include <lua.hpp>

#include "Coroutine.hpp"

namespace gh {

class Pipeline;
class Cancel;

// Expose C++ interface to lua script
class LuaInterface {
public:
  explicit LuaInterface(boost::asio::any_io_executor& executor, Cancel& stopApplication)
      : _Executor(executor), _StopApplication(stopApplication) {}
  ~LuaInterface() {}

  auto GetExecutor() -> boost::asio::any_io_executor { return _Executor; }
  auto GetStopApplication() -> Cancel& { return _StopApplication; }

  void Schedule(std::move_only_function<Omni::Fiber::Coroutine<int>(lua_State*, int)>&&);
  auto Resume(lua_State*, int) -> Omni::Fiber::Coroutine<int>;

private:
  boost::asio::any_io_executor _Executor;
  Cancel& _StopApplication;
  std::optional<std::move_only_function<Omni::Fiber::Coroutine<int>(lua_State*, int)>> _PendingYield;
};

} // namespace gh
