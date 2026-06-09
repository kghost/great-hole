#include "LuaLibCommon.hpp"

#include "Endpoint.hpp"
#include "EndpointUdp.hpp"
#include "EndpointUdpDynMux.hpp"
#include "EndpointUdpMux.hpp"
#include "ErrorCode.hpp"
#include "ResolverCombinedEndpoint.hpp"
#include "ResolverHelper.hpp"

namespace gh {

static void UdpCreateChannel(lua_State* L) {
  auto top = lua_gettop(L);
  if (top < 2 || top > 3) {
    throw std::runtime_error("udp_create_channel: invalid number of arguments");
  }

  auto& u = *(std::shared_ptr<Udp>*)luaL_checkudata(L, 1, kNameUdp);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  std::shared_ptr<ResolverEndpoint> resolver;
  if (top == 2) {
    auto input = luaL_checkstring(L, 2);
    resolver = FindResolverEndpoint(input, u->GetResolveFor());
  } else {
    auto host = luaL_checkstring(L, 2);
    luaL_checkany(L, 3);
    auto port = lua_tostring(L, 3);
    if (!port) {
      throw std::runtime_error("udp_create_channel: port must be a number or a string");
    }
    resolver = std::make_shared<ResolverCombinedEndpoint>(FindResolverIp(host, u->GetResolveFor()),
                                                          FindResolverPort(port, u->GetResolveFor()));
  }

  auto ch = new (lua_newuserdata(L, sizeof(std::shared_ptr<Endpoint>))) std::shared_ptr<Endpoint>();
  luaL_getmetatable(L, kNameEndpoint);
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, u, resolver = std::move(resolver), ch](this auto self, lua_State* L,
                                                                         int nres) -> Omni::Fiber::Coroutine<int> {
    *ch = co_await u->CreateChannel(resolver);
    co_return 1;
  });
}

static void UdpStop(lua_State* L) {
  auto& u = *(std::shared_ptr<Udp>*)luaL_checkudata(L, 1, kNameUdp);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([&interface, u](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await u->Stop();
    co_return 0;
  });
}

static const struct luaL_Reg kUdpMetatable[] = {{"__gc", SafeCall<Gc<Udp, kNameUdp>>},
                                                {"create_channel", SafeYield<UdpCreateChannel>},
                                                {"stop", SafeYield<UdpStop>},
                                                {nullptr, nullptr}};

static void UdpNew(lua_State* L) {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));
  std::shared_ptr<Udp>* udp = nullptr;
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
    throw std::runtime_error("udp: not enough arguments");
  }

  luaL_getmetatable(L, kNameUdp);
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, udp](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    ErrorCode err = co_await (*udp)->Start();
    if (err) {
      throw boost::system::system_error(err, "udp start error");
    }
    co_return 1;
  });
}

// udp-mux-server
static void UdpMuxServerCreateChannel(lua_State* L) {
  auto top = lua_gettop(L);
  if (top < 2 || top > 4) {
    throw std::runtime_error("udp_mux_server_create_channel: invalid number of arguments");
  }

  auto id = (uint8_t)luaL_checknumber(L, 2);
  auto& u = *(std::shared_ptr<UdpMux>*)luaL_checkudata(L, 1, kNameUdpMuxServer);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  std::shared_ptr<ResolverEndpoint> resolver = nullptr;
  if (top == 3) {
    auto input = luaL_checkstring(L, 3);
    resolver = FindResolverEndpoint(input, u->GetResolveFor());
  } else if (top == 4) {
    auto host = luaL_checkstring(L, 3);
    luaL_checkany(L, 4);
    auto port = lua_tostring(L, 4);
    if (!port) {
      throw std::runtime_error("udp_mux_server_create_channel: port must be a number or a string");
    }
    resolver = std::make_shared<ResolverCombinedEndpoint>(FindResolverIp(host, u->GetResolveFor()),
                                                          FindResolverPort(port, u->GetResolveFor()));
  }

  interface.Schedule([&interface, u, id, resolver = std::move(resolver)](this auto self, lua_State* L,
                                                                         int nres) -> Omni::Fiber::Coroutine<int> {
    auto ch = new (lua_newuserdata(L, sizeof(std::shared_ptr<Endpoint>))) std::shared_ptr<Endpoint>();
    luaL_getmetatable(L, kNameEndpoint);
    lua_setmetatable(L, -2);

    if (resolver) {
      *ch = co_await u->CreateChannel(id, resolver);
    } else {
      *ch = co_await u->CreateChannel(id);
    }
    co_return 1;
  });
}

static void UdpMuxServerStop(lua_State* L) {
  auto& u = *(std::shared_ptr<UdpMux>*)luaL_checkudata(L, 1, kNameUdpMuxServer);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([&interface, u](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await u->Stop();
    co_return 0;
  });
}

static const struct luaL_Reg kUdpMuxServerMetatable[] = {{"__gc", SafeCall<Gc<UdpMux, kNameUdpMuxServer>>},
                                                         {"create_channel", SafeYield<UdpMuxServerCreateChannel>},
                                                         {"stop", SafeYield<UdpMuxServerStop>},
                                                         {nullptr, nullptr}};

static void UdpMuxServerNew(lua_State* L) {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));
  std::shared_ptr<UdpMux>* udp = nullptr;
  switch (lua_gettop(L)) {
  case 0:
    udp = new (lua_newuserdata(L, sizeof(std::shared_ptr<UdpMux>)))
        std::shared_ptr<UdpMux>(new UdpMux(interface.GetContext()));
    break;
  case 1: {
    auto bind = boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v6(), (int)lua_tonumber(L, 1));
    udp = new (lua_newuserdata(L, sizeof(std::shared_ptr<UdpMux>)))
        std::shared_ptr<UdpMux>(new UdpMux(interface.GetContext(), bind));
    break;
  }
  default:
    throw std::runtime_error("udp_mux_server: not enough arguments");
  }

  luaL_getmetatable(L, kNameUdpMuxServer);
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, udp](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    ErrorCode err = co_await (*udp)->Start();
    if (err) {
      throw boost::system::system_error(err, "udp_mux_server start error");
    }
    co_return 1;
  });
}

// udp-dyn-mux
static void UdpDynMuxCreateChannel(lua_State* L) {
  auto top = lua_gettop(L);
  if (top < 2 || top > 4) {
    throw std::runtime_error("udp_dyn_mux_create_channel: invalid number of arguments");
  }

  size_t len = 0;
  auto s = luaL_checklstring(L, 2, &len);
  if (len != 16) {
    throw std::runtime_error("udp_dyn_mux_create_channel: psk must be exactly 16 bytes");
  }
  UdpDynMux::PskType psk;
  std::copy_n(reinterpret_cast<const uint8_t*>(s), 16, psk.begin());

  auto& u = *(std::shared_ptr<UdpDynMux>*)luaL_checkudata(L, 1, kNameUdpDynMux);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  std::shared_ptr<ResolverEndpoint> resolver = nullptr;
  if (top == 3) {
    auto input = luaL_checkstring(L, 3);
    resolver = FindResolverEndpoint(input, u->GetResolveFor());
  } else if (top == 4) {
    auto host = luaL_checkstring(L, 3);
    luaL_checkany(L, 4);
    auto port = lua_tostring(L, 4);
    if (!port) {
      throw std::runtime_error("udp_dyn_mux_create_channel: port must be a number or a string");
    }
    resolver = std::make_shared<ResolverCombinedEndpoint>(FindResolverIp(host, u->GetResolveFor()),
                                                          FindResolverPort(port, u->GetResolveFor()));
  }

  auto ch = new (lua_newuserdata(L, sizeof(std::shared_ptr<Endpoint>))) std::shared_ptr<Endpoint>();
  luaL_getmetatable(L, kNameEndpoint);
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, u, psk, resolver = std::move(resolver), ch](this auto self, lua_State* L,
                                                                              int nres) -> Omni::Fiber::Coroutine<int> {
    if (resolver) {
      *ch = co_await u->CreateChannel(psk, resolver);
    } else {
      *ch = co_await u->CreateChannel(psk);
    }
    co_return 1;
  });
}

static void UdpDynMuxStop(lua_State* L) {
  auto& u = *(std::shared_ptr<UdpDynMux>*)luaL_checkudata(L, 1, kNameUdpDynMux);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([&interface, u](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await u->Stop();
    co_return 0;
  });
}

static const struct luaL_Reg kUdpDynMuxMetatable[] = {{"__gc", SafeCall<Gc<UdpDynMux, kNameUdpDynMux>>},
                                                      {"create_channel", SafeYield<UdpDynMuxCreateChannel>},
                                                      {"stop", SafeYield<UdpDynMuxStop>},
                                                      {nullptr, nullptr}};

static void UdpDynMuxNew(lua_State* L) {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));
  std::shared_ptr<UdpDynMux>* udp = nullptr;

  std::shared_ptr<VpnServer> vpnServer = nullptr;
  std::optional<int> port;

  auto top = lua_gettop(L);
  if (top == 1) {
    if (lua_isnumber(L, 1)) {
      port = (int)lua_tonumber(L, 1);
    } else {
      vpnServer = *(std::shared_ptr<VpnServer>*)luaL_checkudata(L, 1, kNameVpnServer);
    }
  } else if (top == 2) {
    port = (int)luaL_checknumber(L, 1);
    vpnServer = *(std::shared_ptr<VpnServer>*)luaL_checkudata(L, 2, kNameVpnServer);
  } else if (top > 2) {
    throw std::runtime_error("udp_dyn_mux: too many arguments");
  }

  UdpDynMux::ChannelNotification& notification =
      vpnServer ? static_cast<UdpDynMux::ChannelNotification&>(*vpnServer)
                : static_cast<UdpDynMux::ChannelNotification&>(UdpDynMux::_NoopChannelNotification);

  if (port.has_value()) {
    auto bind = boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v6(), *port);
    udp = new (lua_newuserdata(L, sizeof(std::shared_ptr<UdpDynMux>)))
        std::shared_ptr<UdpDynMux>(new UdpDynMux(interface.GetContext(), bind, notification));
  } else {
    udp = new (lua_newuserdata(L, sizeof(std::shared_ptr<UdpDynMux>)))
        std::shared_ptr<UdpDynMux>(new UdpDynMux(interface.GetContext(), notification));
  }

  luaL_getmetatable(L, kNameUdpDynMux);
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, udp](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    ErrorCode err = co_await (*udp)->Start();
    if (err) {
      throw boost::system::system_error(err, "udp_dyn_mux start error");
    }
    co_return 1;
  });
}

void RegisterUdp(lua_State* L, LuaInterface& interface) {
  lua_pushlightuserdata(L, &interface);
  lua_pushcclosure(L, SafeYield<UdpNew>, 1);
  lua_setfield(L, -2, "udp");

  lua_pushlightuserdata(L, &interface);
  lua_pushcclosure(L, SafeYield<UdpMuxServerNew>, 1);
  lua_setfield(L, -2, "udp_mux_server");

  lua_pushlightuserdata(L, &interface);
  lua_pushcclosure(L, SafeYield<UdpDynMuxNew>, 1);
  lua_setfield(L, -2, "udp_dyn_mux");

  luaL_newmetatable(L, kNameUdp);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pushlightuserdata(L, &interface);
  luaL_setfuncs(L, kUdpMetatable, 1);
  lua_pop(L, 1);

  luaL_newmetatable(L, kNameUdpMuxServer);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pushlightuserdata(L, &interface);
  luaL_setfuncs(L, kUdpMuxServerMetatable, 1);
  lua_pop(L, 1);

  luaL_newmetatable(L, kNameUdpDynMux);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pushlightuserdata(L, &interface);
  luaL_setfuncs(L, kUdpDynMuxMetatable, 1);
  lua_pop(L, 1);
}

} // namespace gh
