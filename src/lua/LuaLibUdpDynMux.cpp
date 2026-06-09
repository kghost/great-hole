#include "LuaLibCommon.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include <boost/asio.hpp>
#include <boost/system/system_error.hpp>

#include "EndpointUdpDynMux.hpp"
#include "ErrorCode.hpp"
#include "ResolverCombinedEndpoint.hpp"
#include "ResolverHelper.hpp"
#include "VpnServer.hpp"

namespace gh {

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

  auto& u = *LuaUdpDynMux::Get(L, 1);
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

  auto ch = LuaEndpoint::New(L);
  luaL_getmetatable(L, LuaEndpoint::GetTypeTag());
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
  auto& u = *LuaUdpDynMux::Get(L, 1);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([&interface, u](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await u->Stop();
    co_return 0;
  });
}

static const struct luaL_Reg kUdpDynMuxMetatable[] = {{"__gc", SafeCall<LuaUdpDynMux::Gc>},
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
      vpnServer = *LuaVpnServer::Get(L, 1);
    }
  } else if (top == 2) {
    port = (int)luaL_checknumber(L, 1);
    vpnServer = *LuaVpnServer::Get(L, 2);
  } else if (top > 2) {
    throw std::runtime_error("udp_dyn_mux: too many arguments");
  }

  UdpDynMux::ChannelNotification& notification =
      vpnServer ? static_cast<UdpDynMux::ChannelNotification&>(*vpnServer)
                : static_cast<UdpDynMux::ChannelNotification&>(UdpDynMux::_NoopChannelNotification);

  if (port.has_value()) {
    auto bind = boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v6(), *port);
    udp = LuaUdpDynMux::MakeShared(L, interface.GetContext(), bind, notification);
  } else {
    udp = LuaUdpDynMux::MakeShared(L, interface.GetContext(), notification);
  }

  luaL_getmetatable(L, LuaUdpDynMux::GetTypeTag());
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, udp](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    ErrorCode err = co_await (*udp)->Start();
    if (err) {
      throw boost::system::system_error(err, "udp_dyn_mux start error");
    }
    co_return 1;
  });
}

void RegisterUdpDynMux(lua_State* L, LuaInterface& interface) {
  lua_pushlightuserdata(L, &interface);
  lua_pushcclosure(L, SafeYield<UdpDynMuxNew>, 1);
  lua_setfield(L, -2, "udp_dyn_mux");

  luaL_newmetatable(L, LuaUdpDynMux::GetTypeTag());
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pushlightuserdata(L, &interface);
  luaL_setfuncs(L, kUdpDynMuxMetatable, 1);
  lua_pop(L, 1);
}

} // namespace gh
