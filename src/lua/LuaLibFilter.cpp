#include "LuaLibCommon.hpp"

#include <vector>

#include "FilterXor.hpp"

namespace gh {

static const struct luaL_Reg kFilterMetatable[] = {{"__gc", SafeCall<LuaFilter::Gc>}, {nullptr, nullptr}};

static int FilterXorNew(lua_State* L) {
  auto c = lua_gettop(L);
  if (c != 1) {
    return luaL_error(L, "filter_xor: not enough arguments");
  }

  size_t len = 0;
  auto s = lua_tolstring(L, 1, &len);
  if (len < 32 || s == nullptr) {
    return luaL_error(L, "filter_xor: malformed xor key");
  }

  LuaFilter::MakeShared<FilterXor>(L, std::vector<char>(s, s + len));
  luaL_getmetatable(L, LuaFilter::GetTypeTag());
  lua_setmetatable(L, -2);
  return 1;
}

void RegisterFilter(lua_State* L, LuaInterface& /*interface*/) {
  lua_pushcfunction(L, SafeCall<FilterXorNew>);
  lua_setfield(L, -2, "filter_xor");

  luaL_newmetatable(L, LuaFilter::GetTypeTag());
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  luaL_setfuncs(L, kFilterMetatable, 0);
  lua_pop(L, 1);
}

} // namespace gh
