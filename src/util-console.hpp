#ifndef UTIL_CONSOLE_H
#define UTIL_CONSOLE_H

#include <memory>
#include <boost/asio.hpp>

class endpoint_input;
class endpoint_output;

std::shared_ptr<endpoint_input> get_cin(boost::asio::io_service &io_service);
std::shared_ptr<endpoint_output> get_cout(boost::asio::io_service &io_service);
std::shared_ptr<endpoint_output> get_cerr(boost::asio::io_service &io_service);

#endif /* end of include guard: UTIL_CONSOLE_H */
