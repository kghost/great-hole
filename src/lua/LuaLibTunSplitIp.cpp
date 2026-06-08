#include "LuaLibCommon.hpp"

#include "Endpoint.hpp"
#include "EndpointTunSplitIp.hpp"
#include "ErrorCode.hpp"

namespace gh {

static void TunSplitIpNew(lua_State* L) {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));
  std::shared_ptr<EndpointTunSplitIp>* tun = nullptr;
  if (lua_gettop(L) == 1) {
    if (lua_isnumber(L, 1)) {
      tun = new (lua_newuserdata(L, sizeof(std::shared_ptr<EndpointTunSplitIp>)))
          std::shared_ptr<EndpointTunSplitIp>(new EndpointTunSplitIp(interface.GetContext(), (int)lua_tonumber(L, 1)));
    } else {
      tun = new (lua_newuserdata(L, sizeof(std::shared_ptr<EndpointTunSplitIp>)))
          std::shared_ptr<EndpointTunSplitIp>(new EndpointTunSplitIp(interface.GetContext(), lua_tostring(L, 1)));
    }
  } else {
    throw std::runtime_error("tun_split_ip: invalid arguments");
  }

  luaL_getmetatable(L, kNameTunSplitIp);
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, tun](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    ErrorCode err = co_await (*tun)->Start();
    if (err) {
      throw boost::system::system_error(err, "tun_split_ip start error");
    }
    co_return 1;
  });
}

static void TunSplitIpCreateChannel(lua_State* L) {
  auto& tun = *(std::shared_ptr<EndpointTunSplitIp>*)luaL_checkudata(L, 1, kNameTunSplitIp);
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

  auto ch = new (lua_newuserdata(L, sizeof(std::shared_ptr<Endpoint>))) std::shared_ptr<Endpoint>();
  luaL_getmetatable(L, kNameEndpoint);
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, tun, ips = std::move(ips), ch](this auto self, lua_State* L,
                                                                 int nres) -> Omni::Fiber::Coroutine<int> {
    *ch = co_await tun->CreateChannel(ips);
    co_return 1;
  });
}

static void TunSplitIpRemoveChannel(lua_State* L) {
  auto& tun = *(std::shared_ptr<EndpointTunSplitIp>*)luaL_checkudata(L, 1, kNameTunSplitIp);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));
  auto& ch = *(std::shared_ptr<Endpoint>*)luaL_checkudata(L, 2, kNameEndpoint);

  auto tunCh = std::dynamic_pointer_cast<EndpointTunSplitIp::Channel>(ch);
  if (!tunCh) {
    throw std::runtime_error("tun_split_ip remove_channel: invalid channel type");
  }

  interface.Schedule([&interface, tun, tunCh](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await tun->RemoveChannel(tunCh);
    co_return 0;
  });
}

static void TunSplitIpStop(lua_State* L) {
  auto& tun = *(std::shared_ptr<EndpointTunSplitIp>*)luaL_checkudata(L, 1, kNameTunSplitIp);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([&interface, tun](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await tun->Stop();
    co_return 0;
  });
}

static const struct luaL_Reg kTunSplitIpMetatable[] = {
    {"__gc", SafeCall<Gc<EndpointTunSplitIp, kNameTunSplitIp>>},
    {"create_channel", SafeYield<TunSplitIpCreateChannel>},
    {"remove_channel", SafeYield<TunSplitIpRemoveChannel>},
    {"stop", SafeYield<TunSplitIpStop>},
    {nullptr, nullptr}
};

void RegisterTunSplitIp(lua_State* L, LuaInterface& interface) {
  lua_pushlightuserdata(L, &interface);
  lua_pushcclosure(L, SafeYield<TunSplitIpNew>, 1);
  lua_setfield(L, -2, "tun_split_ip");

  luaL_newmetatable(L, kNameTunSplitIp);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pushlightuserdata(L, &interface);
  luaL_setfuncs(L, kTunSplitIpMetatable, 1);
  lua_pop(L, 1);
}

} // namespace gh
