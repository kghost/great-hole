#include "ResolverDnsService.hpp"

#include <random>

#include <boost/log/trivial.hpp>

#include "AresResolver.hpp"

namespace gh {

ResolverDnsService::ResolverDnsService(const std::string& serviceName, ResolveFor& target)
    : _ServiceName(serviceName), _Target(target) {}

auto ResolverDnsService::GetResolverResult() const -> boost::asio::ip::udp::endpoint {
  if (_Endpoints.empty()) {
    return boost::asio::ip::udp::endpoint{};
  }
  static std::random_device rd;
  static std::mt19937 gen(rd());
  std::uniform_int_distribution<size_t> dis(0, _Endpoints.size() - 1);
  return _Endpoints[dis(gen)];
}

auto ResolverDnsService::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> { co_return ErrorCode{}; }

auto ResolverDnsService::DoWork() -> Omni::Fiber::Coroutine<void> {
  auto executor = _Target.GetExecutor();
  auto srvResult = co_await AresResolver::ResolveSrv(executor, _ServiceName, _Service.value()._Stop);
  if (!srvResult.has_value()) {
    _ResolveError = srvResult.error();
    co_return;
  }

  for (auto const& record : srvResult.value()) {
    if (_Service.value()._Stop.IsTriggered()) {
      _ResolveError = make_error_code(boost::asio::error::operation_aborted);
      co_return;
    }
    auto ipResult = co_await AresResolver::ResolveIp(executor, record.Target, _Service.value()._Stop);
    if (!ipResult.has_value()) {
      BOOST_LOG_TRIVIAL(warning) << "Failed to resolve SRV target: " << record.Target << " " << ipResult.error().message();
      continue;
    }
    for (auto const& address : ipResult.value()) {
      _Endpoints.emplace_back(address, record.Port);
    }
  }

  if (_Endpoints.empty()) {
    _ResolveError = make_error_code(boost::asio::error::host_not_found);
    co_return;
  }

  _ResolveError = ErrorCode{};
}

auto ResolverDnsService::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> { co_return ErrorCode{}; }

} // namespace gh
