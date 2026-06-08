#include "LuaLibCommon.hpp"

#include "Endpoint.hpp"
#include "EndpointTun.hpp"
#include "ErrorCode.hpp"

namespace gh {

static void EndpointStop(lua_State* L) {
  auto& ep = *(std::shared_ptr<Endpoint>*)luaL_checkudata(L, 1, kNameEndpoint);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([&interface, ep](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await ep->Stop();
    co_return 0;
  });
}

static const struct luaL_Reg kEndpointMetatable[] = {
    {"__gc", SafeCall<Gc<Endpoint, kNameEndpoint>>},
    {"stop", SafeYield<EndpointStop>},
    {nullptr, nullptr}
};

static void TunNew(lua_State* L) {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  if (lua_gettop(L) != 1) {
    throw std::runtime_error("tun: not enough arguments");
  }

  auto tun = new (lua_newuserdata(L, sizeof(std::shared_ptr<Endpoint>)))
      std::shared_ptr<Endpoint>(new Tun(interface.GetContext(), lua_tostring(L, 1)));
  luaL_getmetatable(L, kNameEndpoint);
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, tun](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    ErrorCode err = co_await (*tun)->Start();
    if (err) {
      throw boost::system::system_error(err, "tun start error");
    }
    co_return 1;
  });
}

void RegisterEndpoint(lua_State* L, LuaInterface& interface) {
  lua_pushlightuserdata(L, &interface);
  lua_pushcclosure(L, SafeYield<TunNew>, 1);
  lua_setfield(L, -2, "tun");

  luaL_newmetatable(L, kNameEndpoint);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pushlightuserdata(L, &interface);
  luaL_setfuncs(L, kEndpointMetatable, 1);
  lua_pop(L, 1);
}

} // namespace gh
