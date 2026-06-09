#pragma once

#include <boost/asio.hpp>
#include <lua.hpp>
#include <memory>

#include "LuaInterface.hpp"

namespace gh {

// Metatable names
inline constexpr char kNamePipeline[] = "Hole.pipeline";
inline constexpr char kNameEndpoint[] = "Hole.endpoint";
inline constexpr char kNameFilter[] = "Hole.filter";
inline constexpr char kNameUdp[] = "Hole.udp";
inline constexpr char kNameUdpMuxServer[] = "Hole.udp-mux-server";
inline constexpr char kNameUdpDynMux[] = "Hole.udp-dyn-mux";
inline constexpr char kNameTunSplitIp[] = "Hole.tun-split-ip";
inline constexpr char kNameVpnServer[] = "Hole.vpn-server";

template <const char* TypeTag, typename Type> class LuaSafeUserData {
public:
  static constexpr const char* GetTypeTag() { return TypeTag; }
  static Type* Get(lua_State* L, int index) { return (Type*)luaL_checkudata(L, index, TypeTag); }
  template <typename... Args> static Type* New(lua_State* L, Args&&... args) {
    return new (lua_newuserdata(L, sizeof(Type))) Type(std::forward<Args>(args)...);
  }
};

template <int F(lua_State* L)> inline int SafeCall(lua_State* L) {
  try {
    return F(L);
  } catch (std::exception const& e) {
    return luaL_error(L, e.what());
  }
}

template <void F(lua_State* L)> inline int SafeYield(lua_State* L) {
  try {
    F(L);
  } catch (std::exception const& e) {
    return luaL_error(L, e.what());
  }
  return lua_yield(L, 0);
}

template <typename T, const char N[]> inline int Gc(lua_State* L) {
  typedef std::shared_ptr<T> P;
  ((P*)luaL_checkudata(L, 1, N))->~P();
  return 0;
}

inline boost::asio::ip::address_v6 GetAddress(const char* str) {
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
void RegisterTunSplitIp(lua_State* L, LuaInterface& interface);
void RegisterVpnServer(lua_State* L, LuaInterface& interface);

} // namespace gh
