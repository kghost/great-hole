#include "ResolverIpDns.hpp"

#include <random>

#include "Utils.hpp"

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

Omni::Fiber::Coroutine<ErrorCode> ResolverIpDns::DoStart() { co_return ErrorCode{}; }

Omni::Fiber::Coroutine<void> ResolverIpDns::DoWork() {
  if (_Service.value()._Stop.IsTriggered()) {
    _ResolveError = make_error_code(boost::asio::error::operation_aborted);
    co_return;
  }
  boost::asio::ip::udp::resolver resolver(_Executor);
  auto [err, results] = co_await resolver.async_resolve(_Host, "", _Service.value()._Stop.AsioSlot()());
  if (err) {
    _ResolveError = err;
    co_return;
  }
  for (auto const& entry : results) {
    _Addresses.push_back(MapToV6(entry.endpoint().address()));
  }
  _ResolveError = ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> ResolverIpDns::DoGracefulStop() { co_return ErrorCode{}; }

} // namespace gh
