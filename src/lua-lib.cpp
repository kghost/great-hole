#include "config.h"

#include "lua-lib.hpp"

#include <memory>
#include <boost/asio/ip/address_v6.hpp>

#include "pipeline.hpp"
#include "filter-xor.hpp"
#include "endpoint-tun.hpp"
#include "endpoint-udp.hpp"
#include "endpoint-udp-mux-server.hpp"
#include "endpoint-udp-mux-client.hpp"

#include "libs/lua-5.3.2/lua.h"
#include "libs/lua-5.3.2/lauxlib.h"


static boost::asio::ip::address_v6 get_address(const char * str)
{
	auto address = boost::asio::ip::make_address(str);
	if (address.is_v4()) {
		return boost::asio::ip::make_address_v6(boost::asio::ip::v4_mapped_t(), address.to_v4());
	} else {
		return address.to_v6();
	}
}

template<int f(lua_State *L)>
static int safe_call(lua_State *L) {
	try {
		return f(L);
	} catch(std::exception const &e) {
		luaL_error(L, e.what());
		return 0;
	}
}

constexpr const char name_pipeline[] = "Hole.pipeline";
constexpr const char name_endpoint[] = "Hole.endpoint";
constexpr const char name_filter[] = "Hole.filter";
constexpr const char name_udp[] = "Hole.udp";
constexpr const char name_udp_mux_server[] = "Hole.udp-mux-server";

template<typename T, const char N[]>
static int gc(lua_State *L) {
	typedef std::shared_ptr<T> P;
	((P*)luaL_checkudata(L, 1, N))->~P();
	return 0;
}

template <typename T, T mf, const char N[]> struct proxy;
template <typename T, void (T::*mf)(), const char N[]>
struct proxy<void (T::*)(), mf, N> {
	static int call(lua_State *L) {
		(*(*(std::shared_ptr<T>*)luaL_checkudata(L, 1, N)).*mf)();
		return 0;
	}
};

static const struct luaL_Reg filter_metatable[] = {
	{"__gc", safe_call<gc<filter, name_filter>>},
	{NULL, NULL}
};

static int filter_xor_new(lua_State *L) {
	auto c = lua_gettop(L);
	if (c != 1) {
		luaL_error(L, "filter_xor: not enough arguments");
		return 0;
	}

	size_t len;
	auto s = lua_tolstring(L, 1, &len);
	if (len < 32 || s == NULL) {
		luaL_error(L, "filter_xor: malformed xor key");
		return 0;
	}

	new (lua_newuserdata(L, sizeof(std::shared_ptr<filter>))) std::shared_ptr<filter>(new filter_xor(std::vector<char>(s, s + len)));
	luaL_getmetatable(L, name_filter);
	lua_setmetatable(L, -2);
	return 1;
}

static const struct luaL_Reg pipeline_metatable[] = {
	{"__gc", safe_call<gc<pipeline, name_pipeline>>},
	{"start", safe_call<&proxy<decltype(&pipeline::start), &pipeline::start, name_pipeline>::call>},
	{"stop", safe_call<&proxy<decltype(&pipeline::stop), &pipeline::stop, name_pipeline>::call>},
	{NULL, NULL}
};

static int pipeline_new(lua_State *L) {
	auto c = lua_gettop(L);
	if (c < 2) {
		luaL_error(L, "pipeline: not enough arguments");
		return 0;
	}

	auto &in = *(std::shared_ptr<endpoint>*)luaL_checkudata(L, 1, name_endpoint);
	auto &out = *(std::shared_ptr<endpoint>*)luaL_checkudata(L, c, name_endpoint);
	std::vector<std::shared_ptr<filter>> filters(c - 2);
	for (auto i = 2; i < c; ++i) {
		filters[i - 2] = *(std::shared_ptr<filter>*)luaL_checkudata(L, i, name_filter);
	}

	new (lua_newuserdata(L, sizeof(std::shared_ptr<pipeline>))) std::shared_ptr<pipeline>(new pipeline(in, filters, out));
	luaL_getmetatable(L, name_pipeline);
	lua_setmetatable(L, -2);
	return 1;
}

static const struct luaL_Reg endpoint_metatable[] = {
	{"__gc", safe_call<gc<endpoint, name_endpoint>>},
	{NULL, NULL}
};

static int udp_create_channel (lua_State *L) {
	if (lua_gettop(L) != 3) {
		luaL_error(L, "udp_create_channel: not enough arguments");
		return 0;
	}

	auto address = lua_tostring(L, 2);
	auto port = (int)lua_tonumber(L, 3);
	auto peer = boost::asio::ip::udp::endpoint(get_address(address), port);

	auto &u = *(std::shared_ptr<udp>*)luaL_checkudata(L, 1, name_udp);

	new (lua_newuserdata(L, sizeof(std::shared_ptr<endpoint>))) std::shared_ptr<endpoint>(u->create_channel(peer));
	luaL_getmetatable(L, name_endpoint);
	lua_setmetatable(L, -2);
	return 1;
}

static const struct luaL_Reg udp_metatable[] = {
	{"__gc", safe_call<gc<udp, name_udp>>},
	{"create_channel", safe_call<udp_create_channel>},
	{NULL, NULL}
};

static int udp_new(lua_State *L) {
	auto &io_service = *(boost::asio::io_service*)lua_touserdata(L, lua_upvalueindex(1));
	switch(lua_gettop(L)) {
		case 0:
			new (lua_newuserdata(L, sizeof(std::shared_ptr<udp>))) std::shared_ptr<udp>(new udp(io_service));
			break;
		case 1:
			{
				auto peer = boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v6(), (int)lua_tonumber(L, 1));
				new (lua_newuserdata(L, sizeof(std::shared_ptr<udp>))) std::shared_ptr<udp>(new udp(io_service, peer));
				break;
			}
		default:
			luaL_error(L, "udp: not enough arguments");
			break;
	}

	luaL_getmetatable(L, name_udp);
	lua_setmetatable(L, -2);

	return 1;
}

// udp-mux-server
static int udp_mux_server_create_channel (lua_State *L) {
	if (lua_gettop(L) != 2) {
		luaL_error(L, "udp_mux_server_create_channel: not enough arguments");
		return 0;
	}

	auto id = (uint8_t)lua_tonumber(L, 2);
	auto &udp = *reinterpret_cast<std::shared_ptr<udp_mux_server>*>(luaL_checkudata(L, 1, name_udp_mux_server));
	new (lua_newuserdata(L, sizeof(std::shared_ptr<endpoint>))) std::shared_ptr<endpoint>(udp->create_channel(id));
	luaL_getmetatable(L, name_endpoint);
	lua_setmetatable(L, -2);
	return 1;
}

static const struct luaL_Reg udp_mux_server_metatable[] = {
	{"__gc", safe_call<gc<udp_mux_server, name_udp_mux_server>>},
	{"create_channel", safe_call<udp_mux_server_create_channel>},
	{NULL, NULL}
};

static int udp_mux_server_new(lua_State *L) {
	auto &io_service = *(boost::asio::io_service*)lua_touserdata(L, lua_upvalueindex(1));
	switch(lua_gettop(L)) {
		case 0:
			new (lua_newuserdata(L, sizeof(std::shared_ptr<udp_mux_server>))) std::shared_ptr<udp_mux_server>(new udp_mux_server(io_service));
			break;
		case 1:
			{
				auto peer = boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v6(), (int)lua_tonumber(L, 1));
				new (lua_newuserdata(L, sizeof(std::shared_ptr<udp_mux_server>))) std::shared_ptr<udp_mux_server>(new udp_mux_server(io_service, peer));
				break;
			}
		default:
			luaL_error(L, "udp_mux_server: not enough arguments");
			break;
	}

	luaL_getmetatable(L, name_udp_mux_server);
	lua_setmetatable(L, -2);

	return 1;
}

// udp-mux-client
static int udp_mux_client_new(lua_State *L) {
	auto &io_service = *(boost::asio::io_service*)lua_touserdata(L, lua_upvalueindex(1));

	switch(lua_gettop(L)) {
		case 3:
			{
				auto id = (int)lua_tonumber(L, 1);
				auto peer_address = lua_tostring(L, 2);
				auto peer_port = (int)lua_tonumber(L, 3);
				auto peer = boost::asio::ip::udp::endpoint(get_address(peer_address), peer_port);
				new (lua_newuserdata(L, sizeof(std::shared_ptr<endpoint>))) std::shared_ptr<endpoint>(new udp_mux_client(io_service, id, peer));
			}
			break;
		case 5:
			{
				auto id = (int)lua_tonumber(L, 1);
				auto peer_address = lua_tostring(L, 2);
				auto peer_port = (int)lua_tonumber(L, 3);
				auto peer = boost::asio::ip::udp::endpoint(get_address(peer_address), peer_port);
				auto local_address = lua_tostring(L, 4);
				auto local_port = (int)lua_tonumber(L, 5);
				auto local = boost::asio::ip::udp::endpoint(get_address(local_address), local_port);
				new (lua_newuserdata(L, sizeof(std::shared_ptr<endpoint>))) std::shared_ptr<endpoint>(new udp_mux_client(io_service, id, peer, local));
				break;
			}
		default:
			luaL_error(L, "udp_mux_client: not enough arguments");
			break;
	}

	luaL_getmetatable(L, name_endpoint);
	lua_setmetatable(L, -2);

	return 1;
}

// tun
static int tun_new(lua_State *L) {
	auto &io_service = *(boost::asio::io_service*)lua_touserdata(L, lua_upvalueindex(1));

	if (lua_gettop(L) != 1) {
		luaL_error(L, "tun: not enough arguments");
		return 0;
	}

	new (lua_newuserdata(L, sizeof(std::shared_ptr<endpoint>))) std::shared_ptr<endpoint>(new tun(io_service, lua_tostring(L, 1)));
	luaL_getmetatable(L, name_endpoint);
	lua_setmetatable(L, -2);
	return 1;
}

static const struct luaL_Reg hole[] = {
	{"pipeline", safe_call<pipeline_new>},
	{"filter_xor", safe_call<filter_xor_new>},
	{NULL, NULL}
};

static const struct luaL_Reg hole_io_object[] = {
	{"tun", safe_call<tun_new>},
	{"udp", safe_call<udp_new>},
	{"udp_mux_server", safe_call<udp_mux_server_new>},
	{"udp_mux_client", safe_call<udp_mux_client_new>},
	{NULL, NULL}
};

static int hole_open(lua_State *L) {
	auto &io_service = *(boost::asio::io_service*)lua_touserdata(L, lua_upvalueindex(1));

	luaL_checkversion(L);
	auto size = (sizeof(hole)/sizeof(hole[0]) - 1) + (sizeof(hole_io_object)/sizeof(hole_io_object[0]) - 1);
	lua_createtable(L, 0, size);
	luaL_setfuncs(L, hole, 0);
	lua_pushlightuserdata(L, &io_service);
	luaL_setfuncs(L, hole_io_object, 1);

	luaL_newmetatable(L, name_udp);
	lua_pushvalue(L, -1);  /* push metatable */
	lua_setfield(L, -2, "__index");  /* metatable.__index = metatable */
	luaL_setfuncs(L, udp_metatable, 0);
	lua_pop(L, 1);

	luaL_newmetatable(L, name_udp_mux_server);
	lua_pushvalue(L, -1);  /* push metatable */
	lua_setfield(L, -2, "__index");  /* metatable.__index = metatable */
	luaL_setfuncs(L, udp_mux_server_metatable, 0);
	lua_pop(L, 1);

	luaL_newmetatable(L, name_endpoint);
	lua_pushvalue(L, -1);  /* push metatable */
	lua_setfield(L, -2, "__index");  /* metatable.__index = metatable */
	luaL_setfuncs(L, endpoint_metatable, 0);
	lua_pop(L, 1);

	luaL_newmetatable(L, name_pipeline);
	lua_pushvalue(L, -1);  /* push metatable */
	lua_setfield(L, -2, "__index");  /* metatable.__index = metatable */
	luaL_setfuncs(L, pipeline_metatable, 0);
	lua_pop(L, 1);

	luaL_newmetatable(L, name_filter);
	lua_pushvalue(L, -1);  /* push metatable */
	lua_setfield(L, -2, "__index");  /* metatable.__index = metatable */
	luaL_setfuncs(L, filter_metatable, 0);
	lua_pop(L, 1);

	return 1;
}

static const char module_name[] = "hole";

void luaopen_hole(lua_State *L, boost::asio::io_service &io_service) {
	luaL_getsubtable(L, LUA_REGISTRYINDEX, "_LOADED");
	lua_getfield(L, -1, module_name);  /* _LOADED[modname] */
	if (!lua_toboolean(L, -1)) {  /* package not already loaded? */
		lua_pop(L, 1);  /* remove field */
		lua_pushlightuserdata(L, &io_service);
		lua_pushcclosure(L, hole_open, 1);
		lua_call(L, 0, 1);  /* call 'openf' to open module */
		lua_pushvalue(L, -1);  /* make copy of module (call result) */
		lua_setfield(L, -3, module_name);  /* _LOADED[modname] = module */
	}
	lua_remove(L, -2);  /* remove _LOADED table */
	lua_setglobal(L, module_name);  /* _G[modname] = module */
}
