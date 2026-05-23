#ifndef UTIL_CONSOLE_H
#define UTIL_CONSOLE_H

#include <boost/asio.hpp>
#include <memory>

class endpoint_input;
class endpoint_output;

std::shared_ptr<endpoint_input> get_cin(boost::asio::io_context& io_context);
std::shared_ptr<endpoint_output> get_cout(boost::asio::io_context& io_context);
std::shared_ptr<endpoint_output> get_cerr(boost::asio::io_context& io_context);

#endif /* end of include guard: UTIL_CONSOLE_H */
