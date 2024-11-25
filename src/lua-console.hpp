#ifndef LUA_CONSOLE_H
#define LUA_CONSOLE_H

#include "lua5.4/lua.h"
#include "lua5.4/lauxlib.h"
#include "lua5.4/lualib.h"

class lua_console : public boost::enable_shared_from_this<lua_console> {
	public:
		explicit lua_console(boost::asio::io_service& io_service) : input_(io_service) {
			input_.assign(STDIN_FILENO);
		}

		void start() {
			l = lua_open();   /* opens Lua */
			luaL_openlibs(l);
		}

		void stop() {
			lua_close(l);
		}

		void read() {
			boost::asio::async_read(
				input_,
				boost::asio::buffer(&_command, sizeof(_command)),
				boost::bind(
					&Input::read_handler,
					shared_from_this(),
					boost::asio::placeholders::error,
					boost::asio::placeholders::bytes_transferred));
		}

	private:
		void read_handler(const gh::error_code& ec, const size_t bytes_transferred) {
			if (ec) {
				std::cerr << "read error: " << std::system_error(ec).what() << std::endl;
				return;
			}

			while (fgets(buff, sizeof(buff), stdin) != NULL) {
				error = luaL_loadbuffer(l, buff, strlen(buff), "line") ||
				lua_pcall(l, 0, 0, 0);
				if (error) {
					fprintf(stderr, "%s", lua_tostring(l, -1));
					lua_pop(l, 1);  /* pop error message from the stack */
				}
			}

			this->read();
		}

	private:
		boost::asio::posix::stream_descriptor input_;
		lua_State *l;
};

#endif /* end of include guard: LUA_CONSOLE_H */
