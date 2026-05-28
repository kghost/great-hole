#include "LuaInterface.hpp"

#include <boost/asio.hpp>
#include <functional>
#include <lua.hpp>
#include <optional>
#include <utility>

#include "Coroutine.hpp"

namespace gh {

void LuaInterface::Schedule(std::move_only_function<Omni::Fiber::Coroutine<int>(lua_State*, int)>&& task) {
  assert(!_PendingYield.has_value());
  _PendingYield.emplace(std::move(task));
}

Omni::Fiber::Coroutine<int> LuaInterface::Yield(lua_State* L, int nres) {
  assert(_PendingYield.has_value());
  co_return co_await std::exchange(_PendingYield, std::nullopt).value()(L, nres);
}

} // namespace gh
