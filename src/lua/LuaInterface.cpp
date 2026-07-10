#include "LuaInterface.hpp"

#include "Coroutine.hpp"

namespace gh {

void LuaInterface::Schedule(std::move_only_function<Omni::Fiber::Coroutine<int>(lua_State*, int)>&& task) {
  assert(!_PendingYield.has_value());
  _PendingYield.emplace(std::move(task));
}

auto LuaInterface::Resume(lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
  assert(_PendingYield.has_value());
  co_return co_await std::exchange(_PendingYield, std::nullopt).value()(L, nres);
}

} // namespace gh
