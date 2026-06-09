#include "ResolverIpDns.hpp"

#include <random>

#include "AresResolver.hpp"

namespace gh {

ResolverIpDns::ResolverIpDns(boost::asio::any_io_executor executor, std::string const& host)
    : _Executor(executor), _Host(host) {}

boost::asio::ip::address_v6 ResolverIpDns::GetResolverResult() const {
  if (_Addresses.empty()) {
    return boost::asio::ip::address_v6{};
  }
  static std::random_device rd;
  static std::mt19937 gen(rd());
  std::uniform_int_distribution<size_t> dis(0, _Addresses.size() - 1);
  return _Addresses[dis(gen)];
}

std::string ResolverIpDns::GetName() const { return std::format("ResolverIpDns:[{}]", _Host); }

Omni::Fiber::Coroutine<ErrorCode> ResolverIpDns::DoStart() { co_return ErrorCode{}; }

Omni::Fiber::Coroutine<void> ResolverIpDns::DoWork() {
  auto result = co_await AresResolver::ResolveIp(_Executor, _Host, _Service.value()._Stop);
  if (!result.has_value()) {
    _ResolveError = result.error();
    co_return;
  }
  _Addresses = std::move(result.value());
  if (_Addresses.empty()) {
    _ResolveError = make_error_code(boost::asio::error::host_not_found);
  } else {
    _ResolveError = ErrorCode{};
  }
}

Omni::Fiber::Coroutine<ErrorCode> ResolverIpDns::DoGracefulStop() { co_return ErrorCode{}; }

} // namespace gh
