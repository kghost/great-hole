#pragma once

#include <memory>
#include <string>

#include <boost/asio.hpp>

#include "Resolver.hpp"

namespace gh {

auto FindResolverIp(const std::string& input, ResolveFor& target) -> std::shared_ptr<ResolverIp>;
auto FindResolverPort(const std::string& input, ResolveFor& target) -> std::shared_ptr<ResolverPort>;
auto FindResolverEndpoint(const std::string& input, ResolveFor& target) -> std::shared_ptr<ResolverEndpoint>;

} // namespace gh
