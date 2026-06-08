#include <vector>

#include "LuaLibCommon.hpp"

#include "EndpointTunSplitIp.hpp"
#include "VpnServer.hpp"

namespace gh {

static int VpnServerNew(lua_State* L) {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));
  auto& tun = *(std::shared_ptr<EndpointTunSplitIp>*)luaL_checkudata(L, 1, kNameTunSplitIp);

  std::vector<std::shared_ptr<Filter>> filters;
  if (lua_gettop(L) >= 2) {
    luaL_checktype(L, 2, LUA_TTABLE);
    auto tableLen = lua_rawlen(L, 2);
    for (unsigned int i = 1; i <= tableLen; ++i) {
      lua_rawgeti(L, 2, i);
      auto& f = *(std::shared_ptr<Filter>*)luaL_checkudata(L, -1, kNameFilter);
      filters.push_back(f);
      lua_pop(L, 1);
    }
  }

  new (lua_newuserdata(L, sizeof(std::shared_ptr<VpnServer>)))
      std::shared_ptr<VpnServer>(new VpnServer(tun, std::move(filters)));
  luaL_getmetatable(L, kNameVpnServer);
  lua_setmetatable(L, -2);
  return 1;
}

static int VpnServerRegisterPeer(lua_State* L) {
  auto& srv = *(std::shared_ptr<VpnServer>*)luaL_checkudata(L, 1, kNameVpnServer);
  size_t len = 0;
  auto pskStr = luaL_checklstring(L, 2, &len);
  if (len != 16) {
    return luaL_error(L, "vpn_server register_peer: psk must be exactly 16 bytes");
  }
  UdpDynMux::PskType psk;
  std::copy_n(reinterpret_cast<const uint8_t*>(pskStr), 16, psk.begin());

  luaL_checktype(L, 3, LUA_TTABLE);
  std::vector<boost::asio::ip::address_v6> ips;
  auto tableLen = lua_rawlen(L, 3);
  for (unsigned int i = 1; i <= tableLen; ++i) {
    lua_rawgeti(L, 3, i);
    auto ipStr = luaL_checkstring(L, -1);
    ips.push_back(GetAddress(ipStr));
    lua_pop(L, 1);
  }

  srv->RegisterPeer(psk, ips);
  return 0;
}

static int VpnServerUnregisterPeer(lua_State* L) {
  auto& srv = *(std::shared_ptr<VpnServer>*)luaL_checkudata(L, 1, kNameVpnServer);
  size_t len = 0;
  auto pskStr = luaL_checklstring(L, 2, &len);
  if (len != 16) {
    return luaL_error(L, "vpn_server unregister_peer: psk must be exactly 16 bytes");
  }
  UdpDynMux::PskType psk;
  std::copy_n(reinterpret_cast<const uint8_t*>(pskStr), 16, psk.begin());

  srv->UnregisterPeer(psk);
  return 0;
}

static void VpnServerRun(lua_State* L) {
  auto& srv = *(std::shared_ptr<VpnServer>*)luaL_checkudata(L, 1, kNameVpnServer);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([&interface, srv](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await srv->Run(interface.GetStopApplication());
    co_return 0;
  });
}

static const struct luaL_Reg kVpnServerMetatable[] = {{"__gc", SafeCall<Gc<VpnServer, kNameVpnServer>>},
                                                      {"register_peer", SafeCall<VpnServerRegisterPeer>},
                                                      {"unregister_peer", SafeCall<VpnServerUnregisterPeer>},
                                                      {"run", SafeYield<VpnServerRun>},
                                                      {nullptr, nullptr}};

void RegisterVpnServer(lua_State* L, LuaInterface& interface) {
  lua_pushlightuserdata(L, &interface);
  lua_pushcclosure(L, SafeCall<VpnServerNew>, 1);
  lua_setfield(L, -2, "vpn_server");

  luaL_newmetatable(L, kNameVpnServer);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pushlightuserdata(L, &interface);
  luaL_setfuncs(L, kVpnServerMetatable, 1);
  lua_pop(L, 1);
}

} // namespace gh
