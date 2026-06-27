#include "LuaLibCommon.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

#include <boost/asio.hpp>
#include <boost/system/system_error.hpp>

#include "EndpointUdpMux.hpp"
#include "ErrorCode.hpp"
#include "ResolverCombinedEndpoint.hpp"
#include "ResolverHelper.hpp"

namespace gh {

static void UdpMuxServerCreateChannel(lua_State* L) {
  auto top = lua_gettop(L);
  if (top < 2 || top > 4) {
    throw std::runtime_error("udp_mux_server_create_channel: invalid number of arguments");
  }

  auto id = (uint8_t)luaL_checknumber(L, 2);
  auto& u = *LuaUdpMux::Get(L, 1);
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

  interface.Schedule(
      [u, id, resolver = std::move(resolver)](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
        auto channel = LuaEndpoint::New(L);
        luaL_getmetatable(L, LuaEndpoint::GetTypeTag());
        lua_setmetatable(L, -2);

        if (resolver) {
          *channel = co_await u->CreateChannel(id, resolver);
        } else {
          *channel = co_await u->CreateChannel(id);
        }
        co_return 1;
      });
}

static void UdpMuxServerStop(lua_State* L) {
  auto& u = *LuaUdpMux::Get(L, 1);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([u](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await u->Stop();
    co_return 0;
  });
}

static void UdpMuxServerStart(lua_State* L) {
  auto& u = *LuaUdpMux::Get(L, 1);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([u](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    ErrorCode err = co_await u->Start();
    if (err) {
      throw boost::system::system_error(err, "udp_mux_server start error");
    }
    co_return 0;
  });
}

static const struct luaL_Reg kUdpMuxServerMetatable[] = {{"__gc", SafeCall<LuaUdpMux::Gc>},
                                                         {"create_channel", SafeYield<UdpMuxServerCreateChannel>},
                                                         {"start", SafeYield<UdpMuxServerStart>},
                                                         {"stop", SafeYield<UdpMuxServerStop>},
                                                         {nullptr, nullptr}};

static int UdpMuxServerNew(lua_State* L) {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));
  switch (lua_gettop(L)) {
  case 0:
    LuaUdpMux::MakeShared(L, interface.GetExecutor());
    break;
  case 1: {
    auto bind = boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v6(), (int)lua_tonumber(L, 1));
    LuaUdpMux::MakeShared(L, interface.GetExecutor(), bind);
    break;
  }
  default:
    throw std::runtime_error("udp_mux_server: not enough arguments");
  }

  luaL_getmetatable(L, LuaUdpMux::GetTypeTag());
  lua_setmetatable(L, -2);
  return 1;
}

void RegisterUdpMux(lua_State* L, LuaInterface& interface) {
  lua_pushlightuserdata(L, &interface);
  lua_pushcclosure(L, SafeCall<UdpMuxServerNew>, 1);
  lua_setfield(L, -2, "udp_mux_server");

  luaL_newmetatable(L, LuaUdpMux::GetTypeTag());
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pushlightuserdata(L, &interface);
  luaL_setfuncs(L, kUdpMuxServerMetatable, 1);
  lua_pop(L, 1);
}

} // namespace gh
