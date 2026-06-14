#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <lua.hpp>

#include "Coroutine.hpp"
#include "LuaInterface.hpp"

namespace gh {

class LuaEngine {
public:
  LuaEngine(boost::asio::any_io_executor executor, Cancel& stopApplication)
      : _Interface(executor, stopApplication), _LuaState(luaL_newstate(), [](lua_State* L) { lua_close(L); }) {
    luaL_openlibs(_LuaState.get());
  }

  Omni::Fiber::Coroutine<void> DoFile(const std::string& filename);

private:
  LuaInterface _Interface;
  std::unique_ptr<lua_State, void (*)(lua_State* L)> _LuaState;

  Omni::Fiber::Coroutine<void> RunLoop(lua_State* co);
};

} // namespace gh
