#include "ResolverIpDns.hpp"

#include <random>
#include <utility>

#ifdef _WIN32
#include "WindowsAsyncResolver.hpp"
#else
#include "AresResolver.hpp"
#endif

namespace gh {
using std::string;

ResolverIpDns::ResolverIpDns(boost::asio::any_io_executor executor, std::string host)
    : _Executor(std::move(executor)), _Host(std::move(host)) {}

auto ResolverIpDns::GetResolverResult() const -> boost::asio::ip::address_v6 {
  if (_Addresses.empty()) {
    return boost::asio::ip::address_v6{};
  }
  static std::random_device randomDevice;
  static std::mt19937 gen(randomDevice());
  std::uniform_int_distribution<size_t> dis(0, _Addresses.size() - 1);
  return _Addresses[dis(gen)];
}

auto ResolverIpDns::GetName() const -> std::string { return std::format("ResolverIpDns:[{}]", _Host); }

auto ResolverIpDns::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> { co_return ErrorCode{}; }

auto ResolverIpDns::DoWork() -> Omni::Fiber::Coroutine<void> {
#ifdef _WIN32
  auto result = co_await WindowsAsyncResolver::ResolveIp(_Executor, _Host, _Service.value()._Stop);
#else
  auto result = co_await AresResolver::ResolveIp(_Executor, _Host, _Service.value()._Stop);
#endif
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
