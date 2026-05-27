#pragma once

#include <memory>
#include <vector>

#include <boost/asio.hpp>
#include <lua.hpp>

#include "Coroutine.hpp"
#include "Event.hpp"

namespace gh {

class Pipeline;

// Expose C++ interface to lua script
class LuaInterface {
public:
  explicit LuaInterface(boost::asio::io_context& io_context, Omni::Fiber::Event<>& stopSignal)
      : _Context(io_context), _StopSignal(stopSignal) {}
  ~LuaInterface() {}

  boost::asio::io_context& GetContext() { return _Context; }

  void Schedule(std::function<Omni::Fiber::Coroutine<void>()>&& task) { _PendingTasks.emplace_back(std::move(task)); }
  void Schedule(std::shared_ptr<Pipeline> pipeline);
  Omni::Fiber::Coroutine<void> SpawnTasks();

private:
  boost::asio::io_context& _Context;
  Omni::Fiber::Event<>& _StopSignal;
  std::vector<std::function<Omni::Fiber::Coroutine<void>()>> _PendingTasks;
  int _TaskCounter = 0;
};

} // namespace gh
