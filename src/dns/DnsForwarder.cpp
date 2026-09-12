#include "DnsForwarder.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/log/trivial.hpp>

#include "ErrorCode.hpp"
#include "InterfaceCommonTypes.hpp"

namespace gh::dns {

namespace {

auto ToDnsEndpoint(const boost::asio::ip::udp::endpoint& endpoint) -> Interface::DnsEndpoint {
  if (endpoint.address().is_v4()) {
    return Interface::DnsEndpoint{
        .Address = Interface::Ip4Address{.Bytes = endpoint.address().to_v4().to_bytes()},
        .Port = endpoint.port(),
    };
  }
  return Interface::DnsEndpoint{
      .Address = Interface::Ip6Address{.Bytes = endpoint.address().to_v6().to_bytes()},
      .Port = endpoint.port(),
  };
}

auto FromDnsEndpoint(const Interface::DnsEndpoint& dnsEndpoint) -> boost::asio::ip::udp::endpoint {
  if (std::holds_alternative<Interface::Ip4Address>(dnsEndpoint.Address)) {
    const auto& ip4 = std::get<Interface::Ip4Address>(dnsEndpoint.Address);
    return {boost::asio::ip::make_address_v4(ip4.Bytes), dnsEndpoint.Port};
  }
  const auto& ip6 = std::get<Interface::Ip6Address>(dnsEndpoint.Address);
  return {boost::asio::ip::make_address_v6(ip6.Bytes), dnsEndpoint.Port};
}

} // namespace

DnsForwarder::DnsForwarder(boost::asio::any_io_executor executor, Interface::DnsForwarderConfiguration config,
                           Interface::DnsForwarderCallbacks& callbacks)
    : _Executor(std::move(executor)) {
  std::unordered_map<std::string, std::shared_ptr<DnsUpstream>> upstreamsByName;
  _Upstreams.reserve(config.Upstreams.size());
  for (const auto& upstreamConfig : config.Upstreams) {
    std::vector<boost::asio::ip::udp::endpoint> servers;
    servers.reserve(upstreamConfig.ServerEndpoints.size());
    for (const auto& serverEndpoint : upstreamConfig.ServerEndpoints) {
      servers.push_back(FromDnsEndpoint(serverEndpoint));
    }
    auto upstream = std::make_shared<DnsUpstream>(_Executor, upstreamConfig.Name, std::move(servers));
    _Upstreams.push_back(upstream);
    upstreamsByName[upstreamConfig.Name] = upstream;
  }

  DnsRouter::Configuration routerConfig;
  if (config.DefaultRoute.has_value()) {
    auto defaultIter = upstreamsByName.find(*config.DefaultRoute);
    if (defaultIter != upstreamsByName.end()) {
      routerConfig.DefaultRoute = defaultIter->second;
    } else {
      BOOST_LOG_TRIVIAL(warning) << "Default route refers to unknown upstream: " << *config.DefaultRoute;
    }
  }

  for (const auto& [domain, upstreamName] : config.Routes) {
    auto routeIter = upstreamsByName.find(upstreamName);
    if (routeIter != upstreamsByName.end()) {
      routerConfig.Routes[domain] = routeIter->second;
    } else {
      BOOST_LOG_TRIVIAL(warning) << "Route for " << domain << " refers to unknown upstream: " << upstreamName;
    }
  }

  _Router = std::make_shared<DnsRouter>(std::move(routerConfig), callbacks);

  _Listeners.reserve(config.Listeners.size());
  for (const auto& listenerConfig : config.Listeners) {
    auto localEndpoint = FromDnsEndpoint(listenerConfig.LocalEndpoint);
    _Listeners.push_back(std::make_shared<DnsListener>(_Executor, localEndpoint, *_Router));
  }
}

DnsForwarder::~DnsForwarder() = default;

auto DnsForwarder::GetConfiguration() const -> Interface::DnsForwarderConfiguration {
  Interface::DnsForwarderConfiguration config;

  config.Listeners.reserve(_Listeners.size());
  for (const auto& listener : _Listeners) {
    config.Listeners.push_back(Interface::DnsListenerConfiguration{
        .LocalEndpoint = ToDnsEndpoint(listener->GetLocalEndpoint()),
    });
  }

  config.Upstreams.reserve(_Upstreams.size());
  for (const auto& upstream : _Upstreams) {
    std::vector<Interface::DnsEndpoint> serverEndpoints;
    serverEndpoints.reserve(upstream->GetUpstreamServers().size());
    for (const auto& serverEndpoint : upstream->GetUpstreamServers()) {
      serverEndpoints.push_back(ToDnsEndpoint(serverEndpoint));
    }
    config.Upstreams.push_back(Interface::DnsUpstreamConfiguration{
        .Name = upstream->GetUpstreamName(),
        .ServerEndpoints = std::move(serverEndpoints),
        .LocalPort = upstream->GetLocalPort(),
    });
  }

  const auto& defaultRoute = _Router->GetDefaultRoute();
  if (defaultRoute.has_value()) {
    if (auto upstream = defaultRoute->lock()) {
      config.DefaultRoute = upstream->GetUpstreamName();
    }
  }

  for (const auto& [domain, upstreamWeak] : _Router->GetRoutes()) {
    if (auto upstream = upstreamWeak.lock()) {
      config.Routes[domain] = upstream->GetUpstreamName();
    }
  }

  return config;
}

auto DnsForwarder::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> {
  std::vector<std::shared_ptr<DnsUpstream>> startedUpstreams;
  for (auto& upstream : _Upstreams) {
    auto err = co_await upstream->Start();
    if (err) {
      for (auto& started : startedUpstreams) {
        co_await started->Stop();
      }
      co_return err;
    }
    startedUpstreams.push_back(upstream);
  }

  auto errRouter = co_await _Router->Start();
  if (errRouter) {
    for (auto& started : startedUpstreams) {
      co_await started->Stop();
    }
    co_return errRouter;
  }

  std::vector<std::shared_ptr<DnsListener>> startedListeners;
  for (auto& listener : _Listeners) {
    auto err = co_await listener->Start();
    if (err) {
      for (auto& started : startedListeners) {
        co_await started->Stop();
      }
      co_await _Router->Stop();
      for (auto& started : startedUpstreams) {
        co_await started->Stop();
      }
      co_return err;
    }
    startedListeners.push_back(listener);
  }

  co_return ErrorCode{};
}

auto DnsForwarder::DoWork() -> Omni::Fiber::Coroutine<void> { co_await _Service.value()._Stop.GetFiberCancelEvent(); }

auto DnsForwarder::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> {
  for (auto& listener : _Listeners) {
    co_await listener->Stop();
  }

  for (auto& upstream : _Upstreams) {
    co_await upstream->Stop();
  }

  co_await _Router->Stop();

  co_return ErrorCode{};
}

} // namespace gh::dns
