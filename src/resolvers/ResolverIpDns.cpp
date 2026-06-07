#include "ResolverIpDns.hpp"

#include <random>

#include "Asio.hpp"
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

Omni::Fiber::Coroutine<ErrorCode> ResolverIpDns::DoStart() {
  if (_Service.value()._Stop.IsTriggered()) {
    co_return make_error_code(boost::asio::error::operation_aborted);
  }
  boost::asio::ip::udp::resolver resolver(_Executor);
  auto [err, results] = co_await resolver.async_resolve(
      _Host, "",
      boost::asio::bind_cancellation_slot(_Service.value()._Stop.AsioSlot().Slot(), Omni::Fiber::AsioUseFiber));
  if (err) {
    co_return err;
  }
  for (auto const& entry : results) {
    _Addresses.push_back(MapToV6(entry.endpoint().address()));
  }
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> ResolverIpDns::DoGracefulStop() { co_return ErrorCode{}; }

} // namespace gh
