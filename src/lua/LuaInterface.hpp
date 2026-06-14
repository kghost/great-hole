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

  boost::asio::any_io_executor GetExecutor() { return _Executor; }
  Cancel& GetStopApplication() { return _StopApplication; }

  void Schedule(std::move_only_function<Omni::Fiber::Coroutine<int>(lua_State*, int)>&&);
  Omni::Fiber::Coroutine<int> Resume(lua_State*, int);

private:
  boost::asio::any_io_executor _Executor;
  Cancel& _StopApplication;
  std::optional<std::move_only_function<Omni::Fiber::Coroutine<int>(lua_State*, int)>> _PendingYield;
};

} // namespace gh
