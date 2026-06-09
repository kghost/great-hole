#pragma once

#include <boost/asio.hpp>
#include <lua.hpp>

namespace gh {

class LuaInterface;

void LuaOpenHole(lua_State* L, LuaInterface& interface);

} // namespace gh
