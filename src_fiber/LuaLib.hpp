#pragma once

#include <boost/asio.hpp>
#include <lua.hpp>

namespace gh {

class LuaInterface;

void luaopen_hole(lua_State* L, LuaInterface& interface);

} // namespace gh
