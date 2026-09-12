#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <variant>

#include <boost/asio.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/asio/signal_set.hpp>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "DnsForwarder.hpp"
#include "DnsForwarderToolConfig.hpp"
#include "GetCurrentOmniFiber.hpp"
#include "Manager.hpp"

namespace {

auto FormatIp(const gh::Interface::Ip4Address& addr) -> std::string {
  return boost::asio::ip::address_v4(addr.Bytes).to_string();
}

auto FormatIp(const gh::Interface::Ip6Address& addr) -> std::string {
  return boost::asio::ip::address_v6(addr.Bytes).to_string();
}

auto FormatIp(const gh::Interface::IpAddress& addr) -> std::string {
  return std::visit([](const auto& address) { return FormatIp(address); }, addr);
}

auto FormatEndpoint(const gh::Interface::DnsEndpoint& endpoint) -> std::string {
  return FormatIp(endpoint.Address) + ":" + std::to_string(endpoint.Port);
}

void PrintConfiguration(const gh::Interface::DnsForwarderConfiguration& config) {
  std::cout << "========================================\n";
  std::cout << "       GreatHole DNS Forwarder          \n";
  std::cout << "========================================\n";
  std::cout << "Inbound Listeners:\n";
  for (const auto& listener : config.Listeners) {
    std::cout << "  - " << FormatEndpoint(listener.LocalEndpoint) << "\n";
  }

  std::cout << "\nUpstream Resolvers:\n";
  for (const auto& upstream : config.Upstreams) {
    std::cout << "  - [" << upstream.Name << "] Ephemeral Source Port: " << upstream.LocalPort << "\n";
    std::cout << "    Servers:\n";
    for (const auto& server : upstream.ServerEndpoints) {
      std::cout << "      * " << FormatEndpoint(server) << "\n";
    }
  }

  std::cout << "\nRouting Policy:\n";
  if (config.DefaultRoute.has_value()) {
    std::cout << "  Default Route: " << *config.DefaultRoute << "\n";
  } else {
    std::cout << "  Default Route: (none)\n";
  }
  if (!config.Routes.empty()) {
    std::cout << "  Domain Suffix Rules:\n";
    for (const auto& [domain, target] : config.Routes) {
      std::cout << "    * " << domain << " -> " << target << "\n";
    }
  }
  std::cout << "========================================\n";
}

class DnsForwarderToolCallbacks : public gh::Interface::DnsForwarderCallbacks {
public:
  void OnDnsQueryResult(const std::string& upstream, const std::string& domain, DnsQueryResult results) override {
    std::visit(
        [&](const auto& ips) -> auto {
          using T = std::decay_t<decltype(ips)>;
          std::string typeStr = std::is_same_v<T, DnsQueryResultA> ? "A" : "AAAA";
          std::cout << "[DNS Query] Upstream: " << (upstream.empty() ? "(none)" : upstream) << " | " << domain << " ("
                    << typeStr << ") -> ";
          if (ips.empty()) {
            std::cout << "(no results)\n";
          } else {
            bool first = true;
            for (const auto& address : ips) {
              if (!first) {
                std::cout << ", ";
              }
              std::cout << FormatIp(address);
              first = false;
            }
            std::cout << "\n";
          }
        },
        results);
  }
};

} // namespace

auto main() -> int {
  auto config = gh::dns::tools::GetDnsForwarderConfig();

  boost::asio::io_context ioContext;
  Omni::Fiber::AsioExecutor executor(ioContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  int exitCode = 0;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    gh::Cancel stopSignal;
    DnsForwarderToolCallbacks callbacks;
    auto forwarder = std::make_shared<gh::dns::DnsForwarder>(ioContext.get_executor(), std::move(config), callbacks);

    auto& current = co_await Omni::Fiber::GetCurrentOmniFiber();

    auto signalFiber = current.Spawn("signals", [&]() -> Omni::Fiber::Coroutine<void> {
      boost::asio::signal_set signals(ioContext.get_executor(), SIGINT, SIGTERM);
      while (!stopSignal.IsTriggered()) {
        auto [err, sig] = co_await signals.async_wait(stopSignal.AsioSlot()());
        if (!err && (sig == SIGINT || sig == SIGTERM)) {
          std::cout << "\n[DnsForwarder] Termination signal received (" << sig << "). Stopping...\n";
          stopSignal.Trigger();
          break;
        }
        if (err == boost::asio::error::operation_aborted) {
          break;
        }
      }
      co_return;
    });

    auto err = co_await forwarder->Start();
    if (err) {
      std::cerr << "[DnsForwarder] Failed to start DNS forwarder: " << err.message() << "\n";
      exitCode = 1;
      stopSignal.Trigger();
      co_await current.Join(signalFiber);
      co_return;
    }

    PrintConfiguration(forwarder->GetConfiguration());
    std::cout << "[DnsForwarder] Service is active. Press Ctrl+C to stop.\n" << std::endl;

    co_await stopSignal.GetFiberCancelEvent();

    std::cout << "[DnsForwarder] Gracefully shutting down...\n";
    co_await forwarder->Stop();
    std::cout << "[DnsForwarder] Stopped successfully.\n";

    co_await current.Join(signalFiber);
    co_return;
  });

  ioContext.run();
  return exitCode;
}
