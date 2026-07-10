#include "ResolverIpDns.hpp"

#include <random>

#include "AresResolver.hpp"

namespace gh {

ResolverIpDns::ResolverIpDns(boost::asio::any_io_executor executor, std::string const& host)
    : _Executor(executor), _Host(host) {}

auto ResolverIpDns::GetResolverResult() const -> boost::asio::ip::address_v6 {
  if (_Addresses.empty()) {
    return boost::asio::ip::address_v6{};
  }
  static std::random_device rd;
  static std::mt19937 gen(rd());
  std::uniform_int_distribution<size_t> dis(0, _Addresses.size() - 1);
  return _Addresses[dis(gen)];
}

auto ResolverIpDns::GetName() const -> std::string { return std::format("ResolverIpDns:[{}]", _Host); }

auto ResolverIpDns::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> { co_return ErrorCode{}; }

auto ResolverIpDns::DoWork() -> Omni::Fiber::Coroutine<void> {
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

auto ResolverIpDns::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> { co_return ErrorCode{}; }

} // namespace gh
