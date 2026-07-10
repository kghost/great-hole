#pragma once

#include <boost/asio.hpp>
#include <lua.hpp>
#include <memory>

#include "LuaInterface.hpp"

#include "EndpointUdpDynMux.hpp"

namespace gh {

template <size_t N> struct FixedString {
  char value[N]{};
  constexpr FixedString(const char (&str)[N]) {
    for (size_t i = 0; i < N; ++i) {
      value[i] = str[i];
    }
  }
};

template <FixedString TypeTag, typename Type> class LuaSafeUserData {
public:
  static constexpr auto GetTypeTag() -> const char* { return TypeTag.value; }
  static auto Get(lua_State* L, int index) -> Type* { return (Type*)luaL_checkudata(L, index, TypeTag.value); }
  template <typename... Args> static auto New(lua_State* L, Args&&... args) -> Type* {
    return new (lua_newuserdata(L, sizeof(Type))) Type(std::forward<Args>(args)...);
  }
  template <typename ElementType = typename Type::element_type, typename... Args>
  static auto MakeShared(lua_State* L, Args&&... args) -> Type* {
    return new (lua_newuserdata(L, sizeof(Type))) Type(std::make_shared<ElementType>(std::forward<Args>(args)...));
  }
  static auto Gc(lua_State* L) -> int {
    Get(L, 1)->~Type();
    return 0;
  }
};

class Pipeline;
class Endpoint;
class Filter;
class Udp;
class UdpMux;
class EndpointTunSplitIp;
class VpnServer;

using LuaPipeline = LuaSafeUserData<"Hole.pipeline", std::shared_ptr<Pipeline>>;
using LuaEndpoint = LuaSafeUserData<"Hole.endpoint", std::shared_ptr<Endpoint>>;
using LuaFilter = LuaSafeUserData<"Hole.filter", std::shared_ptr<Filter>>;
using LuaUdp = LuaSafeUserData<"Hole.udp", std::shared_ptr<Udp>>;
using LuaUdpMux = LuaSafeUserData<"Hole.udp-mux-server", std::shared_ptr<UdpMux>>;
using LuaUdpDynMux = LuaSafeUserData<"Hole.udp-dyn-mux", std::shared_ptr<UdpDynMux>>;
using LuaTunSplitIp = LuaSafeUserData<"Hole.tun-split-ip", std::shared_ptr<EndpointTunSplitIp>>;
using LuaVpnServer = LuaSafeUserData<"Hole.vpn-server", std::shared_ptr<VpnServer>>;
using LuaChannelNotification =
    LuaSafeUserData<"Hole.channel-notification", std::shared_ptr<UdpDynMux::ChannelNotification>>;

template <int F(lua_State* L)> inline auto SafeCall(lua_State* L) -> int {
  try {
    return F(L);
  } catch (std::exception const& e) {
    return luaL_error(L, e.what());
  }
}

template <void F(lua_State* L)> inline auto SafeYield(lua_State* L) -> int {
  try {
    F(L);
  } catch (std::exception const& e) {
    return luaL_error(L, e.what());
  }
  return lua_yield(L, 0);
}

inline auto GetAddress(const char* str) -> boost::asio::ip::address_v6 {
  auto address = boost::asio::ip::make_address(str);
  if (address.is_v4()) {
    return boost::asio::ip::make_address_v6(boost::asio::ip::v4_mapped_t(), address.to_v4());
  } else {
    return address.to_v6();
  }
}

void RegisterFilter(lua_State* L, LuaInterface& interface);
void RegisterPipeline(lua_State* L, LuaInterface& interface);
void RegisterEndpoint(lua_State* L, LuaInterface& interface);
void RegisterUdp(lua_State* L, LuaInterface& interface);
void RegisterUdpMux(lua_State* L, LuaInterface& interface);
void RegisterUdpDynMux(lua_State* L, LuaInterface& interface);
void RegisterTunSplitIp(lua_State* L, LuaInterface& interface);
void RegisterVpnServer(lua_State* L, LuaInterface& interface);

} // namespace gh
