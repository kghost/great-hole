#ifndef LUA_LIB_H
#define LUA_LIB_H

#include "libs/lua-5.3.2/lua.h"

namespace boost {
	namespace asio {
		class io_service;
	}
}

void luaopen_hole(lua_State *L, boost::asio::io_service &io_service);

#endif /* end of include guard: LUA_LIB_H */
