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

auto FormatIp(const gh::Interface::IpAddress& addr) -> std::string {
  if (std::holds_alternative<gh::Interface::Ip4Address>(addr)) {
    return boost::asio::ip::address_v4(std::get<gh::Interface::Ip4Address>(addr).Bytes).to_string();
  }
  return boost::asio::ip::address_v6(std::get<gh::Interface::Ip6Address>(addr).Bytes).to_string();
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

} // namespace

auto main() -> int {
  auto config = gh::dns::tools::GetDnsForwarderConfig();

  boost::asio::io_context ioContext;
  Omni::Fiber::AsioExecutor executor(ioContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  int exitCode = 0;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    gh::Cancel stopSignal;
    auto forwarder = std::make_shared<gh::dns::DnsForwarder>(ioContext.get_executor(), std::move(config));

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
