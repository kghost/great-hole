#include "DnsForwarder.hpp"

#include <boost/log/trivial.hpp>
#include <expected>
#include <memory>
#include <utility>

#include "ErrorCode.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"

namespace gh::dns {

DnsForwarder::DnsForwarder(boost::asio::any_io_executor executor)
    : _Executor(std::move(executor)), _Router(std::make_shared<DnsRouter>()) {}

DnsForwarder::~DnsForwarder() { _ClientRpc.DiscardAndClose(); }

auto DnsForwarder::AddListener(boost::asio::ip::udp::endpoint endpoint)
    -> Omni::Fiber::Coroutine<std::expected<std::weak_ptr<DnsListener>, ErrorCode>> {
  auto listener = std::make_shared<DnsListener>(_Executor, endpoint, *_Router);
  auto response = co_await _ClientRpc.Call([this, listener, endpoint]() -> Omni::Fiber::Coroutine<ErrorCode> {
    _Listeners.insert(listener);
    auto err = co_await listener->Start();
    if (err) {
      BOOST_LOG_TRIVIAL(error) << "Failed to start listener on " << endpoint << ": " << err.message();
      _Listeners.erase(listener);
    }
    co_return err;
  });
  if (response.has_value()) {
    if (response.value()) {
      co_return std::unexpected(response.value());
    }
    co_return listener;
  }
  co_return std::unexpected(SysError(ECANCELED));
}

auto DnsForwarder::RemoveListener(const std::weak_ptr<DnsListener>& weak) -> Omni::Fiber::Coroutine<void> {
  auto listener = weak.lock();
  if (!listener) {
    co_return;
  }
  auto response = co_await _ClientRpc.Call([this, listener]() -> Omni::Fiber::Coroutine<ErrorCode> {
    _Listeners.erase(listener);
    co_await listener->Stop();
    co_return ErrorCode{};
  });
  if (!response.has_value()) {
    BOOST_LOG_TRIVIAL(error) << "Failed to remove listener";
  }
  co_return;
}

auto DnsForwarder::AddUpstream(std::vector<boost::asio::ip::udp::endpoint> upstreamServers)
    -> Omni::Fiber::Coroutine<std::expected<std::weak_ptr<DnsUpstream>, ErrorCode>> {
  auto upstream = std::make_shared<DnsUpstream>(_Executor, std::move(upstreamServers));
  auto response = co_await _ClientRpc.Call([this, upstream]() -> Omni::Fiber::Coroutine<ErrorCode> {
    _Upstreams.insert(upstream);
    auto err = co_await upstream->Start();
    if (err) {
      BOOST_LOG_TRIVIAL(error) << "AddUpstream start client failed: " << err.message();
      _Upstreams.erase(upstream);
    }
    co_return err;
  });
  if (response.has_value()) {
    if (response.value()) {
      co_return std::unexpected(response.value());
    }
    co_return upstream;
  }
  co_return std::unexpected(SysError(ECANCELED));
}

auto DnsForwarder::RemoveUpstream(const std::weak_ptr<DnsUpstream>& weak) -> Omni::Fiber::Coroutine<void> {
  auto upstream = weak.lock();
  if (!upstream) {
    co_return;
  }
  auto response = co_await _ClientRpc.Call([this, upstream]() -> Omni::Fiber::Coroutine<ErrorCode> {
    _Upstreams.erase(upstream);
    co_await upstream->Stop();
    co_return ErrorCode{};
  });
  if (!response.has_value()) {
    BOOST_LOG_TRIVIAL(error) << "Failed to remove upstream";
  }
  co_return;
}

void DnsForwarder::AddRoute(const std::string& domainSuffix, std::weak_ptr<DnsUpstream> upstream) {
  _Router->AddRoute(domainSuffix, std::move(upstream));
}

void DnsForwarder::RemoveRoute(const std::string& domainSuffix) { _Router->RemoveRoute(domainSuffix); }

void DnsForwarder::SetDefaultRoute(std::weak_ptr<DnsUpstream> upstream) {
  _Router->SetDefaultRoute(std::move(upstream));
}

namespace {

auto ToDnsEndpoint(const boost::asio::ip::udp::endpoint& ep) -> Interface::DnsEndpoint {
  if (ep.address().is_v4()) {
    return Interface::DnsEndpoint{
        .Address = Interface::Ip4Address{.Bytes = ep.address().to_v4().to_bytes()},
        .Port = ep.port(),
    };
  }
  return Interface::DnsEndpoint{
      .Address = Interface::Ip6Address{.Bytes = ep.address().to_v6().to_bytes()},
      .Port = ep.port(),
  };
}

} // namespace

auto DnsForwarder::GetConfiguration() const -> Configuration {
  Configuration config;

  config.Listeners.reserve(_Listeners.size());
  for (const auto& listener : _Listeners) {
    if (listener) {
      config.Listeners.push_back(Interface::DnsListenerConfiguration{
          .Listener = listener,
          .LocalEndpoint = ToDnsEndpoint(listener->GetLocalEndpoint()),
      });
    }
  }

  config.Upstreams.reserve(_Upstreams.size());
  for (const auto& upstream : _Upstreams) {
    if (upstream) {
      std::vector<Interface::DnsEndpoint> serverEndpoints;
      serverEndpoints.reserve(upstream->GetUpstreamServers().size());
      for (const auto& ep : upstream->GetUpstreamServers()) {
        serverEndpoints.push_back(ToDnsEndpoint(ep));
      }
      config.Upstreams.push_back(Interface::DnsUpstreamConfiguration{
          .Upstream = upstream,
          .ServerEndpoints = std::move(serverEndpoints),
          .LocalPort = upstream->GetLocalPort(),
      });
    }
  }

  if (_Router) {
    config.DefaultRoute = _Router->GetDefaultRoute();
    config.Routes = _Router->GetRoutes();
  }

  return config;
}

auto DnsForwarder::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> {
  std::vector<std::shared_ptr<DnsUpstream>> clientsToStart(_Upstreams.begin(), _Upstreams.end());
  for (auto& client : clientsToStart) {
    auto err = co_await client->Start();
    if (err) {
      co_return err;
    }
  }

  auto errRouter = co_await _Router->Start();
  if (errRouter) {
    co_return errRouter;
  }

  std::vector<std::shared_ptr<DnsListener>> listenersToStart(_Listeners.begin(), _Listeners.end());
  for (auto& listener : listenersToStart) {
    auto err = co_await listener->Start();
    if (err) {
      co_return err;
    }
  }

  co_return ErrorCode{};
}

auto DnsForwarder::DoWork() -> Omni::Fiber::Coroutine<void> {
  bool stopped = false;
  while (!stopped) {
    auto [stopResult, rpcResult] = co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(), [] -> void {}),
        Omni::Fiber::SelectPair(_ClientRpc.GetServiceAwaitor(), Omni::Fiber::RemoteCall::HandleRequest));

    if (stopResult.has_value() || (rpcResult.has_value() && !rpcResult.value())) {
      stopped = true;
    }
  }
}

auto DnsForwarder::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> {
  _ClientRpc.DiscardAndClose();

  std::vector<std::shared_ptr<DnsListener>> listenersToStop(_Listeners.begin(), _Listeners.end());
  _Listeners.clear();
  for (auto& listener : listenersToStop) {
    co_await listener->Stop();
  }

  std::vector<std::shared_ptr<DnsUpstream>> clientsToStop(_Upstreams.begin(), _Upstreams.end());
  _Upstreams.clear();
  for (auto& client : clientsToStop) {
    co_await client->Stop();
  }

  co_await _Router->Stop();

  co_return ErrorCode{};
}

} // namespace gh::dns
