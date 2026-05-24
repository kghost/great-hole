#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/asio.hpp>
#include <boost/process/v2/process.hpp>

#include "endpoint.hpp"

namespace gh {

class Exec {
public:
  Exec(boost::asio::io_context& ioContext, const std::string& prog, const std::vector<std::string>& args = {},
       const std::map<std::string, std::string>& env = {})
      : _Prog(prog), _Args(args), _Env(env), _IoContext(ioContext) {}
  ~Exec();

  void Run(std::move_only_function<Event>&& handler);
  void Kill();

  std::shared_ptr<EndpointOutput> GetIn();
  std::shared_ptr<EndpointInput> GetOut();
  std::shared_ptr<EndpointInput> GetErr();

private:
  std::string _Prog;
  std::vector<std::string> _Args;
  std::map<std::string, std::string> _Env;

  class Input;
  class Output;
  class Proc;
  friend class Proc;

  boost::asio::io_context& _IoContext;

  std::shared_ptr<EndpointOutput> _In;
  std::shared_ptr<EndpointInput> _Out;
  std::shared_ptr<EndpointInput> _Err;

  std::optional<boost::asio::readable_pipe> _ChildIn;
  std::optional<boost::asio::writable_pipe> _ChildOut;
  std::optional<boost::asio::writable_pipe> _ChildErr;

  std::weak_ptr<Proc> _P;
};

} // namespace gh
