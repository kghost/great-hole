#include "ResolverHelper.hpp"

#include <algorithm>
#include <boost/lexical_cast.hpp>
#include <cctype>
#include <cstdint>

#include "ResolverCombinedEndpoint.hpp"
#include "ResolverDnsService.hpp"
#include "ResolverIpDns.hpp"
#include "ResolverNumberPort.hpp"
#include "ResolverServicePort.hpp"
#include "ResolverStaticIp.hpp"
#include "Utils.hpp"

namespace gh {

namespace {

auto IsNumeric(std::string const& str) -> bool {
  if (str.empty()) {
    return false;
  }
  return std::all_of(str.begin(), str.end(), [](unsigned char c) -> int { return std::isdigit(c); });
}

} // namespace

auto FindResolverIp(const std::string& input, ResolveFor& target) -> std::shared_ptr<ResolverIp> {
  std::string stripped = input;
  if (!stripped.empty() && stripped.front() == '[' && stripped.back() == ']') {
    stripped = stripped.substr(1, stripped.size() - 2);
  }

  try {
    // Try to parse as static IP
    auto address = boost::asio::ip::make_address(stripped);
    return std::make_shared<ResolverStaticIp>(MapToV6(address));
  } catch (const boost::system::system_error&) {
    // If not a static IP, treat as DNS host name
    return std::make_shared<ResolverIpDns>(target.GetExecutor(), input);
  }
}

auto FindResolverPort(const std::string& input, ResolveFor& target) -> std::shared_ptr<ResolverPort> {
  if (IsNumeric(input)) {
    int port = boost::lexical_cast<int>(input);
    if (port < 0 || port > 65535) {
      return std::make_shared<ResolverServicePort>(input);
    }
    return std::make_shared<ResolverNumberPort>(static_cast<uint16_t>(port));
  } else {
    return std::make_shared<ResolverServicePort>(input);
  }
}

auto FindResolverEndpoint(const std::string& input, ResolveFor& target) -> std::shared_ptr<ResolverEndpoint> {
  size_t lastColon = input.find_last_of(':');
  if (lastColon != std::string::npos) {
    size_t rightBracket = input.find(']');
    if (rightBracket != std::string::npos && lastColon < rightBracket) {
      // The last colon is inside IPv6 brackets, so there is no port.
      return std::make_shared<ResolverDnsService>(input, target);
    } else {
      std::string hostPart = input.substr(0, lastColon);
      std::string portPart = input.substr(lastColon + 1);
      return std::make_shared<ResolverCombinedEndpoint>(FindResolverIp(hostPart, target),
                                                        FindResolverPort(portPart, target));
    }
  } else {
    return std::make_shared<ResolverDnsService>(input, target);
  }
}

} // namespace gh
