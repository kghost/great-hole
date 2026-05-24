#pragma once

#include <memory>

#include <boost/asio.hpp>

namespace gh {

class EndpointInput;
class EndpointOutput;

std::shared_ptr<EndpointInput> GetCin(boost::asio::io_context& io_context);
std::shared_ptr<EndpointOutput> GetCout(boost::asio::io_context& io_context);
std::shared_ptr<EndpointOutput> GetCerr(boost::asio::io_context& io_context);

} // namespace gh
