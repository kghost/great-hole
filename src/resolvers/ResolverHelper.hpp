#pragma once

#include <memory>
#include <string>

#include <boost/asio.hpp>

#include "Resolver.hpp"

namespace gh {

std::shared_ptr<ResolverIp> FindResolverIp(const std::string& input, ResolveFor& target);
std::shared_ptr<ResolverPort> FindResolverPort(const std::string& input, ResolveFor& target);
std::shared_ptr<ResolverEndpoint> FindResolverEndpoint(const std::string& input, ResolveFor& target);

} // namespace gh
