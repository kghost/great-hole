#pragma once

#include <expected>
#include <string>
#include <vector>

#include <boost/asio.hpp>

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "ErrorCode.hpp"

namespace gh {

struct SrvResult {
  std::string Target;
  uint16_t Port;
};

class AresResolver {
public:
  static auto
  ResolveIp(boost::asio::any_io_executor executor, const std::string& host, Cancel& cancel) -> Omni::Fiber::Coroutine<std::expected<std::vector<boost::asio::ip::address_v6>, ErrorCode>>;

  static auto
  ResolveSrv(boost::asio::any_io_executor executor, const std::string& serviceName, Cancel& cancel) -> Omni::Fiber::Coroutine<std::expected<std::vector<SrvResult>, ErrorCode>>;
};

} // namespace gh
