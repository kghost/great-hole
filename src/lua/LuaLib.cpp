#include "LuaLib.hpp"

#include <lua.hpp>

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "LuaInterface.hpp"
#include "LuaLibCommon.hpp"

namespace gh {

static void HoleWaitForExit(lua_State* L) {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([&interface](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await interface.GetStopApplication().GetFiberCancelEvent();
    co_return 0;
  });
}

static int HoleOpen(lua_State* L) {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  luaL_checkversion(L);

  // Create the module table
  lua_newtable(L);

  // Register wait_for_exit
  lua_pushlightuserdata(L, &interface);
  lua_pushcclosure(L, SafeYield<HoleWaitForExit>, 1);
  lua_setfield(L, -2, "wait_for_exit");

  // Register all other sub-units
  RegisterFilter(L, interface);
  RegisterPipeline(L, interface);
  RegisterEndpoint(L, interface);
  RegisterUdp(L, interface);
  RegisterTunSplitIp(L, interface);
  RegisterVpnServer(L, interface);

  return 1;
}

static const char kModuleName[] = "hole";

void luaopen_hole(lua_State* L, LuaInterface& interface) {
  luaL_getsubtable(L, LUA_REGISTRYINDEX, "_LOADED");
  lua_getfield(L, -1, kModuleName); /* _LOADED[modname] */
  if (!lua_toboolean(L, -1)) {      /* package not already loaded? */
    lua_pop(L, 1);                  /* remove field */
    lua_pushlightuserdata(L, &interface);
    lua_pushcclosure(L, HoleOpen, 1);
    lua_call(L, 0, 1);                /* call 'openf' to open module */
    lua_pushvalue(L, -1);             /* make copy of module (call result) */
    lua_setfield(L, -3, kModuleName); /* _LOADED[modname] = module */
  }
  lua_remove(L, -2);             /* remove _LOADED table */
  lua_setglobal(L, kModuleName); /* _G[modname] = module */
}

} // namespace gh