#ifndef LUA_LIB_H
#define LUA_LIB_H

#include <boost/asio.hpp>

#include <lua.hpp>

void luaopen_hole(lua_State* L, boost::asio::io_context& io_context);

#endif /* end of include guard: LUA_LIB_H */
