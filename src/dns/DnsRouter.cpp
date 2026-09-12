#include "DnsRouter.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <span>

#include <boost/log/trivial.hpp>

#include "DnsListener.hpp"
#include "DnsPacket.hpp"
#include "DnsUpstream.hpp"
#include "GetCurrentOmniFiber.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"

namespace gh::dns {

DnsRouter::DnsRouter(DnsRouter::Configuration config) : _DefaultRoute(std::move(config.DefaultRoute)) {
  _Routes.reserve(config.Routes.size());
  for (auto&& [domainSuffix, client] : config.Routes) {
    std::string norm = NormalizeDomain(domainSuffix);
    if (norm.empty()) {
      _DefaultRoute = std::move(client);
    } else {
      _Routes[std::move(norm)] = std::move(client);
    }
  }
}

DnsRouter::~DnsRouter() { _Rpc.DiscardAndClose(); }

auto DnsRouter::NormalizeDomain(const std::string& domain) -> std::string {
  std::string norm = domain;
  std::ranges::transform(norm, norm.begin(), [](unsigned char charactor) -> auto { return std::tolower(charactor); });
  if (!norm.empty() && norm.back() == '.') {
    norm.pop_back();
  }
  return norm;
}

auto DnsRouter::Route(const std::string& domain) const -> std::optional<std::shared_ptr<DnsUpstream>> {
  std::string norm = NormalizeDomain(domain);

  auto tryLock = [](const std::weak_ptr<DnsUpstream>& weak) -> std::optional<std::shared_ptr<DnsUpstream>> {
    if (auto shared = weak.lock()) {
      return shared;
    }
    return std::nullopt;
  };

  if (norm.empty()) {
    if (_DefaultRoute) {
      return tryLock(*_DefaultRoute);
    }
    return std::nullopt;
  }

  if (auto matchIt = _Routes.find(norm); matchIt != _Routes.end()) {
    if (auto locked = tryLock(matchIt->second)) {
      return locked;
    }
  }

  size_t dotPos = 0;
  while ((dotPos = norm.find('.', dotPos)) != std::string::npos) {
    std::string suffix = norm.substr(dotPos + 1);
    if (auto matchIt = _Routes.find(suffix); matchIt != _Routes.end()) {
      if (auto locked = tryLock(matchIt->second)) {
        return locked;
      }
    }
    dotPos++;
  }

  if (_DefaultRoute) {
    return tryLock(*_DefaultRoute);
  }

  return std::nullopt;
}

auto DnsRouter::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> { co_return ErrorCode{}; }

auto DnsRouter::DoWork() -> Omni::Fiber::Coroutine<void> {
  auto& currentFiber = co_await Omni::Fiber::GetCurrentOmniFiber();
  bool stopped = false;

  while (!stopped) {
    auto [stopResult, rpcResult, childResult] = co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(), [] -> void {}),
        Omni::Fiber::SelectPair(_Rpc.GetServiceAwaitor(),
                                [](auto req) -> Omni::Fiber::Coroutine<bool> {
                                  if (!req.has_value()) {
                                    co_return false;
                                  }
                                  co_await req.value()();
                                  co_return true;
                                }),
        Omni::Fiber::SelectPair(currentFiber.ChildAwaitor(), [] -> void {}));

    if (stopResult.has_value() || (rpcResult.has_value() && !rpcResult.value())) {
      stopped = true;
    }

    while (auto finishedChild = currentFiber.TryWait()) {
      std::erase_if(_RequestFibers,
                    [&](const RequestContext& ctx) -> bool { return ctx.FiberHandle == *finishedChild; });
    }
  }

  for (auto& ctx : _RequestFibers) {
    if (ctx.CancelToken) {
      ctx.CancelToken->Trigger();
    }
  }
  co_await currentFiber.WaitAll();
  _RequestFibers.clear();
}

auto DnsRouter::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> {
  _Rpc.DiscardAndClose();

  for (auto& ctx : _RequestFibers) {
    if (ctx.CancelToken) {
      ctx.CancelToken->Trigger();
    }
  }

  auto& currentFiber = co_await Omni::Fiber::GetCurrentOmniFiber();
  for (auto& ctx : _RequestFibers) {
    co_await currentFiber.Join(ctx.FiberHandle);
  }
  _RequestFibers.clear();

  co_return ErrorCode{};
}

auto DnsRouter::HandleRequest(DnsListener& listener, boost::asio::ip::udp::endpoint sender, std::vector<uint8_t> data)
    -> Omni::Fiber::Coroutine<void> {
  auto result =
      co_await _Rpc.Call([this, &listener, data = std::move(data), sender]() mutable -> Omni::Fiber::Coroutine<void> {
        auto& currentFiber = co_await Omni::Fiber::GetCurrentOmniFiber();
        auto cancelToken = std::make_shared<Cancel>();
        uint64_t reqId = ++_NextRequestId;
        std::string fiberName = "DnsRequest-" + std::to_string(reqId);
        auto fiber = currentFiber.Spawn(
            std::move(fiberName),
            [this, &listener, data = std::move(data), sender, cancelToken]() mutable -> Omni::Fiber::Coroutine<void> {
              auto parseResult = DnsPacket::Parse(std::span<const uint8_t>(data.data(), data.size()));
              if (!parseResult) {
                co_return;
              }

              std::string qname = parseResult->GetPrimaryQName();
              std::vector<uint8_t> txBuffer;
              if (auto routeTarget = Route(qname); routeTarget.has_value()) {
                auto resolveRes = co_await routeTarget.value()->Resolve(std::move(*parseResult), *cancelToken);
                if (resolveRes) {
                  txBuffer = resolveRes->Serialize();
                  BOOST_LOG_TRIVIAL(info) << "DnsRouter got response " << txBuffer.size() << " bytes for " << qname;
                } else {
                  BOOST_LOG_TRIVIAL(error) << "DnsRouter resolve error: " << resolveRes.error().message();
                  auto errResp = DnsPacket::MakeErrorResponse(parseResult->Header.Id, DnsRCode::ServFail);
                  txBuffer = errResp.Serialize();
                }
              } else {
                BOOST_LOG_TRIVIAL(error) << "DnsRouter no route for " << qname;
                auto errResp = DnsPacket::MakeErrorResponse(parseResult->Header.Id, DnsRCode::Refused);
                txBuffer = errResp.Serialize();
              }

              if (!txBuffer.empty() && !cancelToken->IsTriggered()) {
                co_await listener.SendResponse(sender, std::move(txBuffer));
              }
            });

        _RequestFibers.push_back(RequestContext{.FiberHandle = fiber, .CancelToken = cancelToken});
      });
  if (!result.has_value()) {
    BOOST_LOG_TRIVIAL(error) << "DnsRouter::HandleRequest rpc failed";
  }
}

} // namespace gh::dns
