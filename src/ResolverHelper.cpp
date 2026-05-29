#include "ResolverHelper.hpp"

namespace gh {

std::shared_ptr<ResolverIp> FindResolverIp(boost::asio::io_context& ioContext, std::string const& input) {
  std::string stripped = input;
  if (!stripped.empty() && stripped.front() == '[' && stripped.back() == ']') {
    stripped = stripped.substr(1, stripped.size() - 2);
  }

  try {
    // Try to parse as static IP
    boost::asio::ip::make_address(stripped);
    return std::make_shared<ResolverStaticIp>(stripped);
  } catch (const boost::system::system_error&) {
    // If not a static IP, treat as DNS host name
    return std::make_shared<ResolverStaticDns>(ioContext, input);
  }
}

std::shared_ptr<ResolverPort> FindResolverPort(std::string const& input) {
  return std::make_shared<ResolverStaticPort>(input);
}

std::shared_ptr<ResolverEndpoint> FindResolverEndpoint(boost::asio::io_context& ioContext, std::string const& input,
                                                       std::string const& service, std::string const& protocol) {
  if (!service.empty() && !protocol.empty()) {
    std::string srvName = "_" + service + "._" + protocol + "." + input;
    return std::make_shared<ResolverDnsService>(ioContext, srvName);
  }

  std::string hostPart;
  std::string portPart = "0";

  size_t lastColon = input.find_last_of(':');
  if (lastColon != std::string::npos) {
    size_t rightBracket = input.find(']');
    if (rightBracket != std::string::npos && lastColon < rightBracket) {
      // The last colon is inside IPv6 brackets, so there is no port.
      hostPart = input;
    } else {
      hostPart = input.substr(0, lastColon);
      portPart = input.substr(lastColon + 1);
    }
  } else {
    hostPart = input;
  }

  auto ipRes = FindResolverIp(ioContext, hostPart);
  auto portRes = FindResolverPort(portPart);

  return std::make_shared<ResolverCombinedEndpoint>(ioContext, ipRes, portRes);
}

} // namespace gh
