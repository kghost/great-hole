#include "LuaLibCommon.hpp"

#include <boost/system/system_error.hpp>

#include "EndpointTunSplitIp.hpp"
#include "ErrorCode.hpp"

namespace gh {

static auto TunSplitIpNew(lua_State* L) -> int {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));
  if (lua_gettop(L) == 1) {
    LuaTunSplitIp::MakeShared(L, interface.GetExecutor(), lua_tostring(L, 1));
  } else {
    throw std::runtime_error("tun_split_ip: invalid arguments");
  }

  luaL_getmetatable(L, LuaTunSplitIp::GetTypeTag());
  lua_setmetatable(L, -2);
  return 1;
}

static void TunSplitIpStart(lua_State* L) {
  auto& tun = *LuaTunSplitIp::Get(L, 1);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([tun](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    ErrorCode err = co_await tun->Start();
    if (err) {
      throw boost::system::system_error(err, "tun_split_ip start error");
    }
    co_return 0;
  });
}

static void TunSplitIpCreateChannel(lua_State* L) {
  auto& tun = *LuaTunSplitIp::Get(L, 1);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  luaL_checktype(L, 2, LUA_TTABLE);
  std::vector<boost::asio::ip::address_v6> ips;
  auto len = lua_rawlen(L, 2);
  for (unsigned int i = 1; i <= len; ++i) {
    lua_rawgeti(L, 2, i);
    auto ipStr = luaL_checkstring(L, -1);
    ips.push_back(GetAddress(ipStr));
    lua_pop(L, 1);
  }

  auto channel = LuaEndpoint::New(L);
  luaL_getmetatable(L, LuaEndpoint::GetTypeTag());
  lua_setmetatable(L, -2);

  interface.Schedule(
      [tun, ips = std::move(ips), channel](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
        *channel = co_await tun->CreateChannel(ips);
        co_return 1;
      });
}

static void TunSplitIpRemoveChannel(lua_State* L) {
  auto& tun = *LuaTunSplitIp::Get(L, 1);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));
  auto& channel = *LuaEndpoint::Get(L, 2);

  auto tunChannel = std::dynamic_pointer_cast<EndpointTunSplitIp::Channel>(channel);
  if (!tunChannel) {
    throw std::runtime_error("tun_split_ip remove_channel: invalid channel type");
  }

  interface.Schedule([tun, tunChannel](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await tun->RemoveChannel(tunChannel);
    co_return 0;
  });
}

static void TunSplitIpStop(lua_State* L) {
  auto& tun = *LuaTunSplitIp::Get(L, 1);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([tun](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await tun->Stop();
    co_return 0;
  });
}

static const struct luaL_Reg kTunSplitIpMetatable[] = {{"__gc", SafeCall<LuaTunSplitIp::Gc>},
                                                       {"create_channel", SafeYield<TunSplitIpCreateChannel>},
                                                       {"remove_channel", SafeYield<TunSplitIpRemoveChannel>},
                                                       {"start", SafeYield<TunSplitIpStart>},
                                                       {"stop", SafeYield<TunSplitIpStop>},
                                                       {nullptr, nullptr}};

void RegisterTunSplitIp(lua_State* L, LuaInterface& interface) {
  lua_pushlightuserdata(L, &interface);
  lua_pushcclosure(L, SafeCall<TunSplitIpNew>, 1);
  lua_setfield(L, -2, "tun_split_ip");

  luaL_newmetatable(L, LuaTunSplitIp::GetTypeTag());
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pushlightuserdata(L, &interface);
  luaL_setfuncs(L, kTunSplitIpMetatable, 1);
  lua_pop(L, 1);
}

} // namespace gh
