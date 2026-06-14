#pragma once

#include <memory>

#include <boost/asio.hpp>

namespace gh {

class EndpointInput;
class EndpointOutput;

std::shared_ptr<EndpointInput> GetCin(boost::asio::any_io_executor executor);
std::shared_ptr<EndpointOutput> GetCout(boost::asio::any_io_executor executor);
std::shared_ptr<EndpointOutput> GetCerr(boost::asio::any_io_executor executor);

} // namespace gh
