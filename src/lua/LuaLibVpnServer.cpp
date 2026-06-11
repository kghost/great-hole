#include "LuaLibCommon.hpp"

#include <vector>

#include <boost/system/system_error.hpp>

#include "ErrorCode.hpp"
#include "VpnServer.hpp" // IWYU pragma: keep

namespace gh {

static int VpnServerNew(lua_State* L) {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));
  auto& tun = *LuaTunSplitIp::Get(L, 1);

  std::vector<std::shared_ptr<Filter>> filters;
  if (lua_gettop(L) >= 2) {
    luaL_checktype(L, 2, LUA_TTABLE);
    auto tableLen = lua_rawlen(L, 2);
    for (unsigned int i = 1; i <= tableLen; ++i) {
      lua_rawgeti(L, 2, i);
      auto& f = *LuaFilter::Get(L, -1);
      filters.push_back(f);
      lua_pop(L, 1);
    }
  }

  LuaVpnServer::MakeShared(L, tun, std::move(filters));
  luaL_getmetatable(L, LuaVpnServer::GetTypeTag());
  lua_setmetatable(L, -2);
  return 1;
}

static int VpnServerRegisterPeer(lua_State* L) {
  auto& srv = *LuaVpnServer::Get(L, 1);
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
  auto& srv = *LuaVpnServer::Get(L, 1);
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

static void VpnServerStart(lua_State* L) {
  auto& srv = *LuaVpnServer::Get(L, 1);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([&interface, srv](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    ErrorCode err = co_await srv->Start();
    if (err) {
      throw boost::system::system_error(err, "vpn_server start error");
    }
    co_return 0;
  });
}

static void VpnServerStop(lua_State* L) {
  auto& srv = *LuaVpnServer::Get(L, 1);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([&interface, srv](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await srv->Stop();
    co_return 0;
  });
}

static int VpnServerAsUdpDynMuxChannelNotification(lua_State* L) {
  auto vpn = *LuaVpnServer::Get(L, 1);
  LuaChannelNotification::New(L, vpn);
  luaL_getmetatable(L, LuaChannelNotification::GetTypeTag());
  lua_setmetatable(L, -2);
  return 1;
}

static const struct luaL_Reg kVpnServerMetatable[] = {
    {"__gc", SafeCall<LuaVpnServer::Gc>},
    {"register_peer", SafeCall<VpnServerRegisterPeer>},
    {"unregister_peer", SafeCall<VpnServerUnregisterPeer>},
    {"as_udp_dyn_mux_channel_notification", SafeCall<VpnServerAsUdpDynMuxChannelNotification>},
    {"start", SafeYield<VpnServerStart>},
    {"stop", SafeYield<VpnServerStop>},
    {nullptr, nullptr}};

void RegisterVpnServer(lua_State* L, LuaInterface& interface) {
  lua_pushlightuserdata(L, &interface);
  lua_pushcclosure(L, SafeCall<VpnServerNew>, 1);
  lua_setfield(L, -2, "vpn_server");

  luaL_newmetatable(L, LuaVpnServer::GetTypeTag());
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pushlightuserdata(L, &interface);
  luaL_setfuncs(L, kVpnServerMetatable, 1);
  lua_pop(L, 1);

  static const struct luaL_Reg kChannelNotificationMetatable[] = {
      {"__gc", SafeCall<LuaChannelNotification::Gc>}, {nullptr, nullptr}};

  luaL_newmetatable(L, LuaChannelNotification::GetTypeTag());
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  luaL_setfuncs(L, kChannelNotificationMetatable, 0);
  lua_pop(L, 1);
}

} // namespace gh
