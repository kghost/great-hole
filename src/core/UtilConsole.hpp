#pragma once

#include <memory>

#include <boost/asio.hpp>

namespace gh {

class EndpointInput;
class EndpointOutput;

auto GetCin(boost::asio::any_io_executor executor) -> std::shared_ptr<EndpointInput>;
auto GetCout(boost::asio::any_io_executor executor) -> std::shared_ptr<EndpointOutput>;
auto GetCerr(boost::asio::any_io_executor executor) -> std::shared_ptr<EndpointOutput>;

} // namespace gh
