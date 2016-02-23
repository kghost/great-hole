#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "config.h"

#include <boost/asio.hpp>

#include "lua-lib.hpp"

#include "libs/lua-5.3.2/lua.h"
#include "libs/lua-5.3.2/lauxlib.h"
#include "libs/lua-5.3.2/lualib.h"

extern const char _binary_init_lua_start[];
extern const char _binary_init_lua_end[];

int main (int argc, char **argv) {
	boost::asio::io_service io_service;

	lua_State *L = luaL_newstate();
	luaL_openlibs(L);
	luaopen_hole(L, io_service);

	int error = luaL_loadbuffer(L, _binary_init_lua_start, _binary_init_lua_end - _binary_init_lua_start, "line") || lua_pcall(L, 0, 0, 0);
	if (error) {
		fprintf(stderr, "%s", lua_tostring(L, -1));
		lua_pop(L, 1);  /* pop error message from the stack */
	}

	io_service.run();

	return 0;
}
