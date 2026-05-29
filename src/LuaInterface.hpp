#pragma once

#include <boost/asio.hpp>
#include <lua.hpp>

#include "Coroutine.hpp"
#include "Event.hpp"

namespace gh {

class Pipeline;

// Expose C++ interface to lua script
class LuaInterface {
public:
  explicit LuaInterface(boost::asio::io_context& io_context, Omni::Fiber::Event<>& stop_signal)
      : _Context(io_context), _StopSignal(stop_signal) {}
  ~LuaInterface() {}

  boost::asio::io_context& GetContext() { return _Context; }
  Omni::Fiber::Event<>& GetStopSignal() { return _StopSignal; }

  void Schedule(std::move_only_function<Omni::Fiber::Coroutine<int>(lua_State*, int)>&&);
  Omni::Fiber::Coroutine<int> Yield(lua_State*, int);

private:
  boost::asio::io_context& _Context;
  Omni::Fiber::Event<>& _StopSignal;
  std::optional<std::move_only_function<Omni::Fiber::Coroutine<int>(lua_State*, int)>> _PendingYield;
};

} // namespace gh
