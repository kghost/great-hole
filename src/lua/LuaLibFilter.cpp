#include "LuaLibCommon.hpp"

#include <vector>

#include "FilterXor.hpp"

namespace gh {

static const struct luaL_Reg kFilterMetatable[] = {
    {"__gc", SafeCall<Gc<Filter, kNameFilter>>},
    {nullptr, nullptr}
};

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

  new (lua_newuserdata(L, sizeof(std::shared_ptr<Filter>)))
      std::shared_ptr<Filter>(new FilterXor(std::vector<char>(s, s + len)));
  luaL_getmetatable(L, kNameFilter);
  lua_setmetatable(L, -2);
  return 1;
}

void RegisterFilter(lua_State* L, LuaInterface& /*interface*/) {
  lua_pushcfunction(L, SafeCall<FilterXorNew>);
  lua_setfield(L, -2, "filter_xor");

  luaL_newmetatable(L, kNameFilter);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  luaL_setfuncs(L, kFilterMetatable, 0);
  lua_pop(L, 1);
}

} // namespace gh
