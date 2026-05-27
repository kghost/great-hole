#include <filesystem>
#include <iostream>

#include <boost/asio.hpp>
#include <boost/log/core/core.hpp>
#include <boost/program_options.hpp>
#include <lua.hpp>

#include "Asio.hpp"
#include "Coroutine.hpp"
#include "GetCurrentFiber.hpp"
#include "LuaInterface.hpp"
#include "logging.hpp"
#include "lua-lib.hpp"
#include "util-console.hpp"

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
    Omni::Fiber::Event stop_signal;

    auto cerr = GetCerr(io_context);
    InitLog(cerr);

    LuaInterface interface(io_context, stop_signal);

    std::unique_ptr<lua_State, void (*)(lua_State* L)> L(luaL_newstate(), [](lua_State* L) { lua_close(L); });
    luaL_openlibs(L.get());
    luaopen_hole(L.get(), interface);

    if (luaL_loadbuffer(L.get(), _binary_init_lua_start, _binary_init_lua_end - _binary_init_lua_start,
                        "internal-lua") ||
        lua_pcall(L.get(), 0, 0, 0)) {
      std::cout << lua_tostring(L.get(), -1) << std::endl;
      lua_pop(L.get(), 1); /* pop error message from the stack */
      co_return;
    }

    if (luaL_dofile(L.get(), start.c_str())) {
      std::cout << lua_tostring(L.get(), -1) << std::endl;
      lua_pop(L.get(), 1); /* pop error message from the stack */
      co_return;
    }

    co_await interface.SpawnTasks();

    auto& current = co_await Omni::Fiber::GetCurrentFiber();

    current.Spawn("signals", [&] -> Omni::Fiber::Coroutine<void> {
      boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
      auto& signals_fiber = co_await Omni::Fiber::GetCurrentFiber();
      signals_fiber.Spawn("wait", [&] -> Omni::Fiber::Coroutine<void> {
        auto [ec, signal_number] = co_await signals.async_wait(Omni::Fiber::AsioUseFiber);
        if (!ec) {
          stop_signal.Fire();
        } else {
          throw boost::system::system_error(ec, "error on waiting signal");
        }
      });
      co_await stop_signal;
      co_await signals_fiber.WaitAll();
    });

    co_await (co_await Omni::Fiber::GetCurrentFiber()).WaitAll();
    co_return;
  });

  io_context.run();

  boost::log::core::get()->remove_all_sinks();

  return 0;
}
