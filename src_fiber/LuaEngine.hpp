#pragma once

#include "LuaInterface.hpp"

#include <lua.hpp>

#include "Coroutine.hpp"

namespace gh {

class LuaEngine {
public:
  LuaEngine(boost::asio::io_context& io_context, Omni::Fiber::Event<>& stop_signal)
      : _Interface(io_context, stop_signal), _LuaState(luaL_newstate(), [](lua_State* L) { lua_close(L); }) {
    luaL_openlibs(_LuaState.get());
  }

  Omni::Fiber::Coroutine<void> DoFile(const std::string& filename);

private:
  LuaInterface _Interface;
  std::unique_ptr<lua_State, void (*)(lua_State* L)> _LuaState;

  Omni::Fiber::Coroutine<void> RunLoop(lua_State* co);
};

} // namespace gh
