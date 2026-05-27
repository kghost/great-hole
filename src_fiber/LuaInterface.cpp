#include "LuaInterface.hpp"

#include <utility>

#include <boost/asio.hpp>
#include <lua.hpp>

#include "Coroutine.hpp"
#include "GetCurrentFiber.hpp"
#include "pipeline.hpp"

namespace gh {

void LuaInterface::Schedule(std::shared_ptr<Pipeline> pipeline) {
  Schedule([pipeline, this]() -> Omni::Fiber::Coroutine<void> {
    co_await pipeline->Start(_StopSignal);
  });
}

Omni::Fiber::Coroutine<void> LuaInterface::SpawnTasks() {
  auto& fiber = co_await Omni::Fiber::GetCurrentFiber();
  for (auto& task : std::exchange(_PendingTasks, {})) {
    fiber.Spawn("lua_task" + std::to_string(_TaskCounter++), std::move(task));
  }
}

} // namespace gh
