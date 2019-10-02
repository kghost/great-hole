#ifndef UTIL_EXEC_H
#define UTIL_EXEC_H

#include <memory>
#include <string>
#include <vector>
#include <map>
#include <boost/asio.hpp>
#include <boost/iostreams/device/file_descriptor.hpp>

#include "endpoint.hpp"

class exec {
	public:
		exec(
			boost::asio::io_service &io_service,
			std::string const &prog,
			std::vector<std::string> const &args = {},
			std::map<std::string, std::string> const &env = {}) : io_service(io_service), prog(prog), args(args), env(env) {}
		~exec();

		void run(std::function<event> &&handler);
		void kill();

		std::shared_ptr<endpoint_output> get_in();
		std::shared_ptr<endpoint_input> get_out();
		std::shared_ptr<endpoint_input> get_err();

	private:
		class signal;
		static signal s;

		static boost::iostreams::file_descriptor_source null_stream_source;
		static boost::iostreams::file_descriptor_sink null_stream_sink;

		std::string prog;
		std::vector<std::string> args;
		std::map<std::string, std::string> env;

		class input;
		class output;
		class proc; friend class proc;

		boost::asio::io_service &io_service;

		boost::iostreams::file_descriptor_source child_in = null_stream_source;
		boost::iostreams::file_descriptor_sink child_out = null_stream_sink;
		boost::iostreams::file_descriptor_sink child_err = null_stream_sink;

		std::shared_ptr<endpoint_output> in;
		std::shared_ptr<endpoint_input> out;
		std::shared_ptr<endpoint_input> err;
		std::weak_ptr<proc> p;
};

#endif /* end of include guard: UTIL_EXEC_H */
