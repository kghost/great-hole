#ifndef LUA_LIB_H
#define LUA_LIB_H

#include <boost/asio.hpp>

#include <lua.h>

void luaopen_hole(lua_State *L, boost::asio::io_service &io_service);

#endif /* end of include guard: LUA_LIB_H */
