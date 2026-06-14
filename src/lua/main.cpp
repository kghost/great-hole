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

  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto fiber_root = manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Cancel stopApplication;
    Cancel stopSignals;

    auto& current = co_await Omni::Fiber::GetCurrentFiber();
    auto fiberLua = current.Spawn("LuaEngine", [&io, &stopApplication, &start]() -> Omni::Fiber::Coroutine<void> {
      {
        LuaEngine engine(io.get_executor(), stopApplication);
        co_await engine.DoFile(start);
      }
      auto& fiber = co_await Omni::Fiber::GetCurrentFiber();
      co_await fiber.WaitAll();
    });

    auto fiberSignal = current.Spawn("signals", [&] -> Omni::Fiber::Coroutine<void> {
      boost::asio::signal_set signals(io.get_executor(), SIGINT, SIGTERM, SIGUSR2);
      while (!stopSignals.IsTriggered()) {
        auto [err, signal_number] = co_await signals.async_wait(stopSignals.AsioSlot()());
        if (!err) {
          switch (signal_number) {
          case SIGINT:
            stopApplication.Trigger();
            break;
          case SIGTERM:
            stopApplication.Trigger();
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

    co_await current.Join(fiberLua);
    stopSignals.Trigger();
    co_await current.Join(fiberSignal);
    co_return;
  });

  io.run();

  boost::log::core::get()->remove_all_sinks();

  return 0;
}
