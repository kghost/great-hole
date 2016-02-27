#include "config.h"

#include <memory>

#include <boost/asio.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>


#include "lua-lib.hpp"

#include "libs/lua-5.3.2/lua.h"
#include "libs/lua-5.3.2/lauxlib.h"
#include "libs/lua-5.3.2/lualib.h"

extern const char _binary_init_lua_start[];
extern const char _binary_init_lua_end[];

namespace po = boost::program_options;
namespace fs = boost::filesystem;

int main (int ac, char **av) {
	po::options_description desc("Options");
	desc.add_options()
	("help", "print this message")
	("startlua", po::value<std::string>(), "the lua script run at start");
	;

	po::positional_options_description pd;
	pd.add("startlua", 1);

	po::variables_map vm;
	try {
		po::store(po::command_line_parser(ac, av).options(desc).positional(pd).run(), vm);
		po::notify(vm);
	} catch (const po::error &ex) {
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

	boost::asio::io_service io_service;

	std::unique_ptr<lua_State, void (*)(lua_State *L)> L(luaL_newstate(), [](lua_State *L) { lua_close(L); });
	luaL_openlibs(L.get());
	luaopen_hole(L.get(), io_service);

	if (luaL_loadbuffer(L.get(), _binary_init_lua_start, _binary_init_lua_end - _binary_init_lua_start, "internal-lua") || lua_pcall(L.get(), 0, 0, 0)) {
		std::cout << lua_tostring(L.get(), -1) << std::endl;
		lua_pop(L.get(), 1);  /* pop error message from the stack */
		return 1;
	}

	if (luaL_dofile(L.get(), start.c_str())) {
		std::cout << lua_tostring(L.get(), -1) << std::endl;
		lua_pop(L.get(), 1);  /* pop error message from the stack */
		return 1;
	}

	io_service.run();

	return 0;
}
