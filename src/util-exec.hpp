#ifndef UTIL_EXEC_H
#define UTIL_EXEC_H

#include <boost/asio.hpp>
#include <boost/process/v2/process.hpp>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "endpoint.hpp"

class exec {
public:
  exec(boost::asio::io_context& io_context, std::string const& prog, std::vector<std::string> const& args = {},
       std::map<std::string, std::string> const& env = {})
      : io_context(io_context), prog(prog), args(args), env(env) {}
  ~exec();

  void run(std::move_only_function<event>&& handler);
  void kill();

  std::shared_ptr<endpoint_output> get_in();
  std::shared_ptr<endpoint_input> get_out();
  std::shared_ptr<endpoint_input> get_err();

private:
  std::string prog;
  std::vector<std::string> args;
  std::map<std::string, std::string> env;

  class input;
  class output;
  class proc;
  friend class proc;

  boost::asio::io_context& io_context;

  std::shared_ptr<endpoint_output> in;
  std::shared_ptr<endpoint_input> out;
  std::shared_ptr<endpoint_input> err;

  std::optional<boost::asio::readable_pipe> child_in;
  std::optional<boost::asio::writable_pipe> child_out;
  std::optional<boost::asio::writable_pipe> child_err;

  std::weak_ptr<proc> p;
};

#endif /* end of include guard: UTIL_EXEC_H */
