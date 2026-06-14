#include "LuaLibCommon.hpp"

#include <boost/system/system_error.hpp>

#include "EndpointTun.hpp"
#include "ErrorCode.hpp"

namespace gh {

static void EndpointStart(lua_State* L) {
  auto& ep = *LuaEndpoint::Get(L, 1);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([&interface, ep](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    ErrorCode err = co_await ep->Start();
    if (err) {
      throw boost::system::system_error(err, "tun start error");
    }
    co_return 0;
  });
}

static void EndpointStop(lua_State* L) {
  auto& ep = *LuaEndpoint::Get(L, 1);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([&interface, ep](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await ep->Stop();
    co_return 0;
  });
}

static const struct luaL_Reg kEndpointMetatable[] = {{"__gc", SafeCall<LuaEndpoint::Gc>},
                                                     {"start", SafeYield<EndpointStart>},
                                                     {"stop", SafeYield<EndpointStop>},
                                                     {nullptr, nullptr}};

static int TunNew(lua_State* L) {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  if (lua_gettop(L) != 1) {
    throw std::runtime_error("tun: not enough arguments");
  }

  LuaEndpoint::MakeShared<Tun>(L, interface.GetExecutor(), lua_tostring(L, 1));
  luaL_getmetatable(L, LuaEndpoint::GetTypeTag());
  lua_setmetatable(L, -2);
  return 1;
}

void RegisterEndpoint(lua_State* L, LuaInterface& interface) {
  lua_pushlightuserdata(L, &interface);
  lua_pushcclosure(L, SafeCall<TunNew>, 1);
  lua_setfield(L, -2, "tun");

  luaL_newmetatable(L, LuaEndpoint::GetTypeTag());
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pushlightuserdata(L, &interface);
  luaL_setfuncs(L, kEndpointMetatable, 1);
  lua_pop(L, 1);
}

} // namespace gh
