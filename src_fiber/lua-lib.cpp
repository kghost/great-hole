#include "lua-lib.hpp"
#include <array>
#include <lua.hpp>

#include <boost/asio/ip/address_v6.hpp>
#include <memory>

#include "Coroutine.hpp"
#include "endpoint-tun.hpp"
// #include "endpoint-udp-dyn-mux-client.hpp"
// #include "endpoint-udp-dyn-mux-server.hpp"
// #include "endpoint-udp-mux-client.hpp"
// #include "endpoint-udp-mux-server.hpp"
#include "endpoint-udp.hpp"
#include "error-code.hpp"
#include "filter-xor.hpp"
#include "pipeline.hpp"

namespace gh {

static boost::asio::ip::address_v6 get_address(const char* str) {
  auto address = boost::asio::ip::make_address(str);
  if (address.is_v4()) {
    return boost::asio::ip::make_address_v6(boost::asio::ip::v4_mapped_t(), address.to_v4());
  } else {
    return address.to_v6();
  }
}

template <int f(lua_State* L)> static int safe_call(lua_State* L) {
  try {
    return f(L);
  } catch (std::exception const& e) {
    return luaL_error(L, e.what());
  }
}

constexpr const char name_pipeline[] = "Hole.pipeline";
constexpr const char name_endpoint[] = "Hole.endpoint";
constexpr const char name_filter[] = "Hole.filter";
constexpr const char name_udp[] = "Hole.udp";
// constexpr const char name_udp_mux_server[] = "Hole.udp-mux-server";
// constexpr const char name_udp_dyn_mux_server[] = "Hole.udp-dyn-mux-server";

template <typename T, const char N[]> static int gc(lua_State* L) {
  typedef std::shared_ptr<T> P;
  ((P*)luaL_checkudata(L, 1, N))->~P();
  return 0;
}

static const struct luaL_Reg filter_metatable[] = {{"__gc", safe_call<gc<Filter, name_filter>>}, {NULL, NULL}};

static int filter_xor_new(lua_State* L) {
  auto c = lua_gettop(L);
  if (c != 1) {
    return luaL_error(L, "filter_xor: not enough arguments");
  }

  size_t len;
  auto s = lua_tolstring(L, 1, &len);
  if (len < 32 || s == NULL) {
    return luaL_error(L, "filter_xor: malformed xor key");
  }

  new (lua_newuserdata(L, sizeof(std::shared_ptr<Filter>)))
      std::shared_ptr<Filter>(new FilterXor(std::vector<char>(s, s + len)));
  luaL_getmetatable(L, name_filter);
  lua_setmetatable(L, -2);
  return 1;
}

static auto pipeline_metatable = std::to_array<const struct luaL_Reg>(
    {{.name = "__gc", .func = safe_call<gc<Pipeline, name_pipeline>>}, {.name = NULL, .func = NULL}});

static int pipeline_new(lua_State* L) {
  auto c = lua_gettop(L);
  if (c < 2) {
    return luaL_error(L, "pipeline: not enough arguments");
  }

  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  std::shared_ptr<EndpointInput> in = *(std::shared_ptr<Endpoint>*)luaL_checkudata(L, 1, name_endpoint);
  std::vector<std::shared_ptr<Filter>> filters(c - 2);
  for (auto i = 2; i < c; ++i) {
    filters[i - 2] = *(std::shared_ptr<Filter>*)luaL_checkudata(L, i, name_filter);
  }
  std::shared_ptr<EndpointOutput> out = *(std::shared_ptr<Endpoint>*)luaL_checkudata(L, c, name_endpoint);

  auto pipe = new (lua_newuserdata(L, sizeof(std::shared_ptr<Pipeline>)))
      std::shared_ptr<Pipeline>(new Pipeline(in, filters, out));
  luaL_getmetatable(L, name_pipeline);
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, pipe](lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    ErrorCode err = co_await (*pipe)->Start(interface.GetStopSignal());
    if (err) {
      throw boost::system::system_error(err, "udp start error");
    }
    co_return 1;
  });

  return lua_yield(L, 0);
}

static const struct luaL_Reg endpoint_metatable[] = {{"__gc", safe_call<gc<Endpoint, name_endpoint>>}, {NULL, NULL}};

static int udp_create_channel(lua_State* L) {
  if (lua_gettop(L) != 3) {
    return luaL_error(L, "udp_create_channel: not enough arguments");
  }

  auto address = lua_tostring(L, 2);
  auto port = (int)lua_tonumber(L, 3);
  auto peer = boost::asio::ip::udp::endpoint(get_address(address), port);

  auto& u = *(std::shared_ptr<Udp>*)luaL_checkudata(L, 1, name_udp);

  new (lua_newuserdata(L, sizeof(std::shared_ptr<Endpoint>))) std::shared_ptr<Endpoint>(u->CreateChannel(peer));
  luaL_getmetatable(L, name_endpoint);
  lua_setmetatable(L, -2);
  return 1;
}

static const struct luaL_Reg udp_metatable[] = {
    {"__gc", safe_call<gc<Udp, name_udp>>}, {"create_channel", safe_call<udp_create_channel>}, {NULL, NULL}};

static int udp_new(lua_State* L) {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));
  std::shared_ptr<Udp>* udp;
  switch (lua_gettop(L)) {
  case 0:
    udp = new (lua_newuserdata(L, sizeof(std::shared_ptr<Udp>))) std::shared_ptr<Udp>(new Udp(interface.GetContext()));
    break;
  case 1: {
    auto bind = boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v6(), (int)lua_tonumber(L, 1));
    udp = new (lua_newuserdata(L, sizeof(std::shared_ptr<Udp>)))
        std::shared_ptr<Udp>(new Udp(interface.GetContext(), bind));
    break;
  }
  default:
    return luaL_error(L, "udp: not enough arguments");
  }

  luaL_getmetatable(L, name_udp);
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, udp](lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    ErrorCode err = co_await (*udp)->Start(interface.GetStopSignal());
    if (err) {
      throw boost::system::system_error(err, "udp start error");
    }
    co_return 1;
  });

  return lua_yield(L, 0);
}

// // udp-mux-server
// static int udp_mux_server_create_channel(lua_State* L) {
//   if (lua_gettop(L) != 2) {
//     return luaL_error(L, "udp_mux_server_create_channel: not enough arguments");
//   }

//   auto id = (uint8_t)lua_tonumber(L, 2);
//   auto& udp = *reinterpret_cast<std::shared_ptr<UdpMuxServer>*>(luaL_checkudata(L, 1, name_udp_mux_server));
//   new (lua_newuserdata(L, sizeof(std::shared_ptr<Endpoint>))) std::shared_ptr<Endpoint>(udp->CreateChannel(id));
//   luaL_getmetatable(L, name_endpoint);
//   lua_setmetatable(L, -2);
//   return 1;
// }

// static const struct luaL_Reg udp_mux_server_metatable[] = {{"__gc", safe_call<gc<UdpMuxServer,
// name_udp_mux_server>>},
//                                                            {"create_channel",
//                                                            safe_call<udp_mux_server_create_channel>}, {NULL, NULL}};

// static int udp_mux_server_new(lua_State* L) {
//   auto& io_context = *(boost::asio::io_context*)lua_touserdata(L, lua_upvalueindex(1));
//   switch (lua_gettop(L)) {
//   case 0:
//     new (lua_newuserdata(L, sizeof(std::shared_ptr<UdpMuxServer>)))
//         std::shared_ptr<UdpMuxServer>(new UdpMuxServer(io_context));
//     break;
//   case 1: {
//     auto peer = boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v6(), (int)lua_tonumber(L, 1));
//     new (lua_newuserdata(L, sizeof(std::shared_ptr<UdpMuxServer>)))
//         std::shared_ptr<UdpMuxServer>(new UdpMuxServer(io_context, peer));
//     break;
//   }
//   default:
//     return luaL_error(L, "udp_mux_server: not enough arguments");
//   }

//   luaL_getmetatable(L, name_udp_mux_server);
//   lua_setmetatable(L, -2);

//   return 1;
// }

// // udp-mux-client
// static int udp_mux_client_new(lua_State* L) {
//   auto& io_context = *(boost::asio::io_context*)lua_touserdata(L, lua_upvalueindex(1));

//   switch (lua_gettop(L)) {
//   case 3: {
//     auto id = (int)lua_tonumber(L, 1);
//     auto peer_address = lua_tostring(L, 2);
//     auto peer_port = (int)lua_tonumber(L, 3);
//     auto peer = boost::asio::ip::udp::endpoint(get_address(peer_address), peer_port);
//     new (lua_newuserdata(L, sizeof(std::shared_ptr<Endpoint>)))
//         std::shared_ptr<Endpoint>(new UdpMuxClient(io_context, id, peer));
//   } break;
//   case 5: {
//     auto id = (int)lua_tonumber(L, 1);
//     auto peer_address = lua_tostring(L, 2);
//     auto peer_port = (int)lua_tonumber(L, 3);
//     auto peer = boost::asio::ip::udp::endpoint(get_address(peer_address), peer_port);
//     auto local_address = lua_tostring(L, 4);
//     auto local_port = (int)lua_tonumber(L, 5);
//     auto local = boost::asio::ip::udp::endpoint(get_address(local_address), local_port);
//     new (lua_newuserdata(L, sizeof(std::shared_ptr<Endpoint>)))
//         std::shared_ptr<Endpoint>(new UdpMuxClient(io_context, id, peer, local));
//     break;
//   }
//   default:
//     return luaL_error(L, "udp_mux_client: not enough arguments");
//   }

//   luaL_getmetatable(L, name_endpoint);
//   lua_setmetatable(L, -2);

//   return 1;
// }

// // udp-dyn-mux-server
// static const struct luaL_Reg udp_dyn_mux_server_metatable[] = {
//     {"__gc", safe_call<gc<UdpDynMuxServer, name_udp_dyn_mux_server>>}, {NULL, NULL}};

// static int udp_dyn_mux_server_new(lua_State* L) {
//   auto& io_context = *(boost::asio::io_context*)lua_touserdata(L, lua_upvalueindex(1));
//   switch (lua_gettop(L)) {
//   case 0:
//     new (lua_newuserdata(L, sizeof(std::shared_ptr<UdpDynMuxServer>)))
//         std::shared_ptr<UdpDynMuxServer>(new UdpDynMuxServer(io_context));
//     break;
//   case 1: {
//     auto peer = boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v6(), (int)lua_tonumber(L, 1));
//     new (lua_newuserdata(L, sizeof(std::shared_ptr<UdpDynMuxServer>)))
//         std::shared_ptr<UdpDynMuxServer>(new UdpDynMuxServer(io_context, peer));
//     break;
//   }
//   default:
//     return luaL_error(L, "udp_dyn_mux_server: not enough arguments");
//   }

//   luaL_getmetatable(L, name_udp_dyn_mux_server);
//   lua_setmetatable(L, -2);

//   return 1;
// }

// // udp-dyn-mux-client
// static int udp_dyn_mux_client_new(lua_State* L) {
//   auto& io_context = *(boost::asio::io_context*)lua_touserdata(L, lua_upvalueindex(1));

//   switch (lua_gettop(L)) {
//   case 2: {
//     auto peer_address = lua_tostring(L, 1);
//     auto peer_port = (int)lua_tonumber(L, 2);
//     auto peer = boost::asio::ip::udp::endpoint(get_address(peer_address), peer_port);
//     new (lua_newuserdata(L, sizeof(std::shared_ptr<Endpoint>)))
//         std::shared_ptr<Endpoint>(new UdpDynMuxClient(io_context, peer));
//     break;
//   }
//   case 4: {
//     auto peer_address = lua_tostring(L, 1);
//     auto peer_port = (int)lua_tonumber(L, 2);
//     auto peer = boost::asio::ip::udp::endpoint(get_address(peer_address), peer_port);
//     auto local_address = lua_tostring(L, 3);
//     auto local_port = (int)lua_tonumber(L, 4);
//     auto local = boost::asio::ip::udp::endpoint(get_address(local_address), local_port);
//     new (lua_newuserdata(L, sizeof(std::shared_ptr<Endpoint>)))
//         std::shared_ptr<Endpoint>(new UdpDynMuxClient(io_context, peer, local));
//     break;
//   }
//   default:
//     return luaL_error(L, "udp_dyn_mux_client: not enough arguments");
//   }

//   luaL_getmetatable(L, name_endpoint);
//   lua_setmetatable(L, -2);

//   return 1;
// }

// =========================== tun ===========================
static int tun_new(lua_State* L) {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  if (lua_gettop(L) != 1) {
    return luaL_error(L, "tun: not enough arguments");
  }

  auto tun = new (lua_newuserdata(L, sizeof(std::shared_ptr<Endpoint>)))
      std::shared_ptr<Endpoint>(new Tun(interface.GetContext(), lua_tostring(L, 1)));
  luaL_getmetatable(L, name_endpoint);
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, tun](lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    ErrorCode err = co_await (*tun)->Start(interface.GetStopSignal());
    if (err) {
      throw boost::system::system_error(err, "tun start error");
    }
    co_return 1;
  });

  return lua_yield(L, 0);
}

// =========================== pipeline ===========================
static auto hole = std::to_array<const struct luaL_Reg>({
    {.name = "filter_xor", .func = safe_call<filter_xor_new>},
    {.name = NULL, .func = NULL},
});

// =========================== pipe io object ===========================
static auto hole_io_object = std::to_array<const struct luaL_Reg>({
    {.name = "pipeline", .func = safe_call<pipeline_new>},
    {.name = "tun", .func = safe_call<tun_new>},
    {.name = "udp", .func = safe_call<udp_new>},
    // {"udp_mux_server", safe_call<udp_mux_server_new>},
    // {"udp_mux_client", safe_call<udp_mux_client_new>},
    // {"udp_dyn_mux_server", safe_call<udp_dyn_mux_server_new>},
    // {"udp_dyn_mux_client", safe_call<udp_dyn_mux_client_new>},
    {.name = NULL, .func = NULL},
});

static int hole_open(lua_State* L) {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  luaL_checkversion(L);
  auto size = hole.size() - 1 + hole_io_object.size() - 1;
  lua_createtable(L, 0, size);
  luaL_setfuncs(L, hole.data(), 0);
  lua_pushlightuserdata(L, &interface);
  luaL_setfuncs(L, hole_io_object.data(), 1);

  luaL_newmetatable(L, name_udp);
  lua_pushvalue(L, -1);           /* push metatable */
  lua_setfield(L, -2, "__index"); /* metatable.__index = metatable */
  luaL_setfuncs(L, udp_metatable, 0);
  lua_pop(L, 1);

  // luaL_newmetatable(L, name_udp_mux_server);
  // lua_pushvalue(L, -1);           /* push metatable */
  // lua_setfield(L, -2, "__index"); /* metatable.__index = metatable */
  // luaL_setfuncs(L, udp_mux_server_metatable, 0);
  // lua_pop(L, 1);

  // luaL_newmetatable(L, name_udp_dyn_mux_server);
  // lua_pushvalue(L, -1);           /* push metatable */
  // lua_setfield(L, -2, "__index"); /* metatable.__index = metatable */
  // luaL_setfuncs(L, udp_dyn_mux_server_metatable, 0);
  // lua_pop(L, 1);

  luaL_newmetatable(L, name_endpoint);
  lua_pushvalue(L, -1);           /* push metatable */
  lua_setfield(L, -2, "__index"); /* metatable.__index = metatable */
  luaL_setfuncs(L, endpoint_metatable, 0);
  lua_pop(L, 1);

  luaL_newmetatable(L, name_pipeline);
  lua_pushvalue(L, -1);           /* push metatable */
  lua_setfield(L, -2, "__index"); /* metatable.__index = metatable */
  luaL_setfuncs(L, pipeline_metatable.data(), 0);
  lua_pop(L, 1);

  luaL_newmetatable(L, name_filter);
  lua_pushvalue(L, -1);           /* push metatable */
  lua_setfield(L, -2, "__index"); /* metatable.__index = metatable */
  luaL_setfuncs(L, filter_metatable, 0);
  lua_pop(L, 1);

  return 1;
}

static const char module_name[] = "hole";

void luaopen_hole(lua_State* L, LuaInterface& interface) {
  luaL_getsubtable(L, LUA_REGISTRYINDEX, "_LOADED");
  lua_getfield(L, -1, module_name); /* _LOADED[modname] */
  if (!lua_toboolean(L, -1)) {      /* package not already loaded? */
    lua_pop(L, 1);                  /* remove field */
    lua_pushlightuserdata(L, &interface);
    lua_pushcclosure(L, hole_open, 1);
    lua_call(L, 0, 1);                /* call 'openf' to open module */
    lua_pushvalue(L, -1);             /* make copy of module (call result) */
    lua_setfield(L, -3, module_name); /* _LOADED[modname] = module */
  }
  lua_remove(L, -2);             /* remove _LOADED table */
  lua_setglobal(L, module_name); /* _G[modname] = module */
}

} // namespace gh