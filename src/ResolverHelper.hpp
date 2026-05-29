#pragma once

#include <memory>
#include <string>

#include <boost/asio.hpp>

#include "Resolver.hpp"

namespace gh {

std::shared_ptr<ResolverIp> FindResolverIp(boost::asio::io_context& ioContext, std::string const& input);

std::shared_ptr<ResolverPort> FindResolverPort(std::string const& input);

std::shared_ptr<ResolverEndpoint> FindResolverEndpoint(boost::asio::io_context& ioContext,
                                                       std::string const& input,
                                                       std::string const& service = "",
                                                       std::string const& protocol = "");

} // namespace gh
