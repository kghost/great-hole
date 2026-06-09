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
  explicit LuaInterface(boost::asio::io_context& io_context, Cancel& stopApplication)
      : _Context(io_context), _StopApplication(stopApplication) {}
  ~LuaInterface() {}

  boost::asio::io_context& GetContext() { return _Context; }
  Cancel& GetStopApplication() { return _StopApplication; }

  void Schedule(std::move_only_function<Omni::Fiber::Coroutine<int>(lua_State*, int)>&&);
  Omni::Fiber::Coroutine<int> Resume(lua_State*, int);

private:
  boost::asio::io_context& _Context;
  Cancel& _StopApplication;
  std::optional<std::move_only_function<Omni::Fiber::Coroutine<int>(lua_State*, int)>> _PendingYield;
};

} // namespace gh
