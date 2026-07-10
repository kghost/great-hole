#include "LuaLibCommon.hpp"

#include <array>
#include <vector>

#include <boost/system/system_error.hpp>

#include "Endpoint.hpp"
#include "Filter.hpp"
#include "Pipeline.hpp" // IWYU pragma: keep

namespace gh {

static void PipelineStart(lua_State* L) {
  auto& pipe = *LuaPipeline::Get(L, 1);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([pipe](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    ErrorCode err = co_await pipe->Start();
    if (err) {
      throw boost::system::system_error(err, "pipeline start error");
    }
    co_return 0;
  });
}

static void PipelineStop(lua_State* L) {
  auto& pipe = *LuaPipeline::Get(L, 1);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([pipe](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await pipe->Stop();
    co_return 0;
  });
}

static const auto kPipelineMetatable = std::to_array<const struct luaL_Reg>({
    {.name = "__gc", .func = SafeCall<LuaPipeline::Gc>},
    {.name = "start", .func = SafeYield<PipelineStart>},
    {.name = "stop", .func = SafeYield<PipelineStop>},
    {.name = nullptr, .func = nullptr},
});

static auto PipelineNew(lua_State* L) -> int {
  auto c = lua_gettop(L);
  if (c < 2) {
    throw std::runtime_error("pipeline: not enough arguments");
  }

  std::shared_ptr<Endpoint> ep1 = *LuaEndpoint::Get(L, 1);
  std::vector<std::shared_ptr<Filter>> filters(c - 2);
  for (auto i = 2; i < c; ++i) {
    filters[i - 2] = *LuaFilter::Get(L, i);
  }
  std::shared_ptr<Endpoint> ep2 = *LuaEndpoint::Get(L, c);

  LuaPipeline::MakeShared(L, ep1, filters, ep2);
  luaL_getmetatable(L, LuaPipeline::GetTypeTag());
  lua_setmetatable(L, -2);
  return 1;
}

void RegisterPipeline(lua_State* L, LuaInterface& interface) {
  lua_pushlightuserdata(L, &interface);
  lua_pushcclosure(L, SafeCall<PipelineNew>, 1);
  lua_setfield(L, -2, "pipeline");

  luaL_newmetatable(L, LuaPipeline::GetTypeTag());
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pushlightuserdata(L, &interface);
  luaL_setfuncs(L, kPipelineMetatable.data(), 1);
  lua_pop(L, 1);
}

} // namespace gh
