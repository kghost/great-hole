#include "LuaLibCommon.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

#include <boost/asio.hpp>
#include <boost/system/system_error.hpp>

#include "EndpointUdp.hpp"
#include "ErrorCode.hpp"
#include "ResolverCombinedEndpoint.hpp"
#include "ResolverHelper.hpp"

namespace gh {

static void UdpCreateChannel(lua_State* L) {
  auto top = lua_gettop(L);
  if (top < 2 || top > 3) {
    throw std::runtime_error("udp_create_channel: invalid number of arguments");
  }

  auto& u = *LuaUdp::Get(L, 1);
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

  auto channel = LuaEndpoint::New(L);
  luaL_getmetatable(L, LuaEndpoint::GetTypeTag());
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, u, resolver = std::move(resolver), channel](this auto self, lua_State* L,
                                                                              int nres) -> Omni::Fiber::Coroutine<int> {
    *channel = co_await u->CreateChannel(resolver);
    co_return 1;
  });
}

static void UdpStop(lua_State* L) {
  auto& u = *LuaUdp::Get(L, 1);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([&interface, u](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await u->Stop();
    co_return 0;
  });
}

static const struct luaL_Reg kUdpMetatable[] = {{"__gc", SafeCall<LuaUdp::Gc>},
                                                {"create_channel", SafeYield<UdpCreateChannel>},
                                                {"stop", SafeYield<UdpStop>},
                                                {nullptr, nullptr}};

static void UdpNew(lua_State* L) {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));
  std::shared_ptr<Udp>* udp = nullptr;
  switch (lua_gettop(L)) {
  case 0:
    udp = LuaUdp::MakeShared(L, interface.GetContext());
    break;
  case 1: {
    auto bind = boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v6(), (int)lua_tonumber(L, 1));
    udp = LuaUdp::MakeShared(L, interface.GetContext(), bind);
    break;
  }
  default:
    throw std::runtime_error("udp: not enough arguments");
  }

  luaL_getmetatable(L, LuaUdp::GetTypeTag());
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, udp](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    ErrorCode err = co_await (*udp)->Start();
    if (err) {
      throw boost::system::system_error(err, "udp start error");
    }
    co_return 1;
  });
}

void RegisterUdp(lua_State* L, LuaInterface& interface) {
  lua_pushlightuserdata(L, &interface);
  lua_pushcclosure(L, SafeYield<UdpNew>, 1);
  lua_setfield(L, -2, "udp");

  luaL_newmetatable(L, LuaUdp::GetTypeTag());
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pushlightuserdata(L, &interface);
  luaL_setfuncs(L, kUdpMetatable, 1);
  lua_pop(L, 1);
}

} // namespace gh
