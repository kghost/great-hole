#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <lua.hpp>

#include "Coroutine.hpp"
#include "LuaInterface.hpp"

namespace gh {

class LuaEngine {
public:
  LuaEngine(boost::asio::any_io_executor executor, Cancel& stopApplication)
      : _Interface(executor, stopApplication), _LuaState(luaL_newstate(), [](lua_State* L) -> void { lua_close(L); }) {
    luaL_openlibs(_LuaState.get());
  }

  auto DoFile(const std::string& filename) -> Omni::Fiber::Coroutine<void>;

private:
  LuaInterface _Interface;
  std::unique_ptr<lua_State, void (*)(lua_State* L)> _LuaState;

  auto RunLoop(lua_State* co) -> Omni::Fiber::Coroutine<void>;
};

} // namespace gh
