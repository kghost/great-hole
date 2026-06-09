#include "LuaEngine.hpp"

#include <iostream>

#include "LuaLib.hpp"

extern const char _binary_init_lua_start[];
extern const char _binary_init_lua_end[];

namespace gh {

Omni::Fiber::Coroutine<void> LuaEngine::RunLoop(lua_State* co) {
  int nres = 0;
  int status = lua_resume(co, _LuaState.get(), 0, &nres);
  while (true) {
    switch (status) {
    case LUA_YIELD: {
      auto nargs = co_await _Interface.Resume(co, nres);
      status = lua_resume(co, _LuaState.get(), nargs, &nres);
      break;
    }
    case LUA_OK:
      co_return;
    default:
      std::cout << lua_tostring(co, -1) << std::endl;
      throw std::runtime_error("Failed to load start lua");
    }
  }
}

Omni::Fiber::Coroutine<void> LuaEngine::DoFile(const std::string& filename) {
  luaL_openlibs(_LuaState.get());

  lua_State* co = lua_newthread(_LuaState.get());
  luaL_openlibs(co);
  LuaOpenHole(co, _Interface);

  if (luaL_loadbuffer(co, _binary_init_lua_start, _binary_init_lua_end - _binary_init_lua_start, "internal-lua") !=
      LUA_OK) {
    std::cout << lua_tostring(co, -1) << std::endl;
    lua_pop(co, 1);
    throw std::runtime_error("Failed to load internal lua");
  }

  co_await RunLoop(co);

  if (luaL_loadfile(co, filename.c_str()) != LUA_OK) {
    std::cout << lua_tostring(co, -1) << std::endl;
    lua_pop(co, 1);
    throw std::runtime_error("Failed to load start lua");
  }

  co_await RunLoop(co);
}

} // namespace gh