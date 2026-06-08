#include "LuaLibCommon.hpp"

#include <array>
#include <vector>

#include "Endpoint.hpp"
#include "Filter.hpp"
#include "Pipeline.hpp"

namespace gh {

static void PipelineStop(lua_State* L) {
  auto& pipe = *(std::shared_ptr<Pipeline>*)luaL_checkudata(L, 1, kNamePipeline);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([&interface, pipe](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await pipe->Stop();
    co_return 0;
  });
}

static const auto kPipelineMetatable = std::to_array<const struct luaL_Reg>({
    {.name = "__gc", .func = SafeCall<Gc<Pipeline, kNamePipeline>>},
    {.name = "stop", .func = SafeYield<PipelineStop>},
    {.name = nullptr, .func = nullptr},
});

static void PipelineNew(lua_State* L) {
  auto c = lua_gettop(L);
  if (c < 2) {
    throw std::runtime_error("pipeline: not enough arguments");
  }

  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  std::shared_ptr<EndpointInput> in = *(std::shared_ptr<Endpoint>*)luaL_checkudata(L, 1, kNameEndpoint);
  std::vector<std::shared_ptr<Filter>> filters(c - 2);
  for (auto i = 2; i < c; ++i) {
    filters[i - 2] = *(std::shared_ptr<Filter>*)luaL_checkudata(L, i, kNameFilter);
  }
  std::shared_ptr<EndpointOutput> out = *(std::shared_ptr<Endpoint>*)luaL_checkudata(L, c, kNameEndpoint);

  auto pipe = new (lua_newuserdata(L, sizeof(std::shared_ptr<Pipeline>)))
      std::shared_ptr<Pipeline>(new Pipeline(in, filters, out));
  luaL_getmetatable(L, kNamePipeline);
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, pipe](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    ErrorCode err = co_await (*pipe)->Start();
    if (err) {
      throw boost::system::system_error(err, "pipeline start error");
    }
    co_return 1;
  });
}

void RegisterPipeline(lua_State* L, LuaInterface& interface) {
  lua_pushlightuserdata(L, &interface);
  lua_pushcclosure(L, SafeYield<PipelineNew>, 1);
  lua_setfield(L, -2, "pipeline");

  luaL_newmetatable(L, kNamePipeline);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pushlightuserdata(L, &interface);
  luaL_setfuncs(L, kPipelineMetatable.data(), 1);
  lua_pop(L, 1);
}

} // namespace gh
