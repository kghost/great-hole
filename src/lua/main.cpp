#include <csignal>
#include <filesystem>
#include <iostream>

#include <boost/asio.hpp>
#include <boost/log/core/core.hpp>
#include <boost/program_options.hpp>
#include <lua.hpp>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "GetCurrentFiber.hpp"
#include "LuaEngine.hpp"
#include "StackTrace.hpp"

extern const char _binary_init_lua_start[];
extern const char _binary_init_lua_end[];

namespace po = boost::program_options;
namespace fs = std::filesystem;

using namespace gh;

int main(int ac, char** av) {
  po::options_description desc("Options");
  desc.add_options()("help", "print this message")("startlua", po::value<std::string>(), "the lua script run at start");

  po::positional_options_description pd;
  pd.add("startlua", 1);

  po::variables_map vm;
  try {
    po::store(po::command_line_parser(ac, av).options(desc).positional(pd).run(), vm);
    po::notify(vm);
  } catch (const po::error& ex) {
    std::cout << ex.what() << std::endl;
    return 1;
  }

  if (vm.count("help") || !vm.count("startlua")) {
    std::cout << "Usage: " << av[0] << " startlua" << std::endl;
    std::cout << std::endl;
    std::cout << desc << std::endl;
    return 1;
  }

  auto start = vm["startlua"].as<std::string>();
  try {
    if (!fs::exists(start) || !fs::is_regular_file(start)) {
      std::cout << "can't load startlua: " << start << std::endl;
      return 1;
    }
  } catch (const fs::filesystem_error& ex) {
    std::cout << ex.what() << std::endl;
    return 1;
  }

  boost::asio::io_context io_context;
  Omni::Fiber::AsioExecutor executor(io_context);
  Omni::Fiber::Manager manager(executor);

  auto fiber_root = manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Cancel stop_signal;

    auto& current = co_await Omni::Fiber::GetCurrentFiber();
    current.Spawn("LuaEngine", [&io_context, &stop_signal, &start]() mutable -> Omni::Fiber::Coroutine<void> {
      {
        LuaEngine engine(io_context, stop_signal.GetFiberCancelEvent());
        co_await engine.DoFile(start);
      }
      auto& fiber = co_await Omni::Fiber::GetCurrentFiber();
      co_await fiber.WaitAll();
    });

    current.Spawn("signals", [&] -> Omni::Fiber::Coroutine<void> {
      auto& signals_fiber = co_await Omni::Fiber::GetCurrentFiber();
      signals_fiber.Spawn("wait", [&io_context, &stop_signal] -> Omni::Fiber::Coroutine<void> {
        boost::asio::signal_set signals(io_context, SIGINT, SIGTERM, SIGUSR2);
        while (!stop_signal.IsTriggered()) {
          auto [err, signal_number] = co_await signals.async_wait(stop_signal.AsioSlot()());
          if (!err) {
            switch (signal_number) {
            case SIGINT:
              stop_signal.Trigger();
              break;
            case SIGTERM:
              stop_signal.Trigger();
              break;
#ifndef NDEBUG
            case SIGUSR2:
              co_await Omni::Fiber::StackTraceAllFibers();
              break;
#endif
            }
          } else if (err == boost::asio::error::operation_aborted) {
            continue;
          } else {
            throw boost::system::system_error(err, "error on waiting signal");
          }
        }
        signals.clear();
      });

      co_await stop_signal.GetFiberCancelEvent();
      co_await signals_fiber.WaitAll();
    });

    co_await (co_await Omni::Fiber::GetCurrentFiber()).WaitAll();
    co_return;
  });

  io_context.run();

  boost::log::core::get()->remove_all_sinks();

  return 0;
}
