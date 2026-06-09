#include "ResolverCombinedEndpoint.hpp"

#include "Event.hpp"
#include "GetCurrentFiber.hpp"
#include "Select.hpp"

namespace gh {

ResolverCombinedEndpoint::ResolverCombinedEndpoint(std::shared_ptr<ResolverIp> ipResolver,
                                                   std::shared_ptr<ResolverPort> portResolver)
    : _IpResolver(ipResolver), _PortResolver(portResolver) {}

boost::asio::ip::udp::endpoint ResolverCombinedEndpoint::GetResolverResult() const { return _Endpoint; }

std::string ResolverCombinedEndpoint::GetName() const {
  return "ResolverCombinedEndpoint:[" + _IpResolver->GetName() + "]:" + _PortResolver->GetName();
}

Omni::Fiber::Coroutine<ErrorCode> ResolverCombinedEndpoint::DoStart() { co_return ErrorCode{}; }

Omni::Fiber::Coroutine<void> ResolverCombinedEndpoint::DoWork() {
  auto& fiber = co_await Omni::Fiber::GetCurrentFiber();

  // 1. Resolve IP
  auto eventPtrIp = std::make_shared<Omni::Fiber::Event<std::expected<boost::asio::ip::address_v6, ErrorCode>>>();
  auto resolveIpFiber = fiber.Spawn("ResolveIP", [this, eventPtrIp]() -> Omni::Fiber::Coroutine<void> {
    auto res = co_await _IpResolver->Resolve(_Service.value()._Stop);
    eventPtrIp->Fire(res);
    co_return;
  });

  bool cancelIp = false;
  std::expected<boost::asio::ip::address_v6, ErrorCode> resIp;
  co_await Omni::Fiber::Select(
      Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(), [&]() { cancelIp = true; }),
      Omni::Fiber::SelectPair(*eventPtrIp, [&](auto const& res) { resIp = res; }));

  if (cancelIp) {
    co_await _IpResolver->Stop();
    co_await fiber.Join(resolveIpFiber);
    _ResolveError = make_error_code(boost::asio::error::operation_aborted);
    co_return;
  }

  co_await fiber.Join(resolveIpFiber);

  if (!resIp.has_value()) {
    _ResolveError = resIp.error();
    co_return;
  }

  // 2. Resolve Port
  auto eventPtrPort = std::make_shared<Omni::Fiber::Event<std::expected<uint16_t, ErrorCode>>>();
  auto resolvePortFiber = fiber.Spawn("ResolvePort", [this, eventPtrPort]() -> Omni::Fiber::Coroutine<void> {
    auto res = co_await _PortResolver->Resolve(_Service.value()._Stop);
    eventPtrPort->Fire(res);
    co_return;
  });

  bool cancelPort = false;
  std::expected<uint16_t, ErrorCode> resPort;
  co_await Omni::Fiber::Select(
      Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(), [&]() { cancelPort = true; }),
      Omni::Fiber::SelectPair(*eventPtrPort, [&](auto const& res) { resPort = res; }));

  if (cancelPort) {
    co_await _PortResolver->Stop();
    co_await fiber.Join(resolvePortFiber);
    _ResolveError = make_error_code(boost::asio::error::operation_aborted);
    co_return;
  }

  co_await fiber.Join(resolvePortFiber);

  if (!resPort.has_value()) {
    _ResolveError = resPort.error();
    co_return;
  }

  _Endpoint = boost::asio::ip::udp::endpoint(resIp.value(), resPort.value());
  _ResolveError = ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> ResolverCombinedEndpoint::DoGracefulStop() {
  if (_IpResolver->GetState() != ServiceBase::State::kNone) {
    co_await _IpResolver->Stop();
    co_await _IpResolver->WaitService();
  }
  if (_PortResolver->GetState() != ServiceBase::State::kNone) {
    co_await _PortResolver->Stop();
    co_await _PortResolver->WaitService();
  }
  co_return ErrorCode{};
}

} // namespace gh
