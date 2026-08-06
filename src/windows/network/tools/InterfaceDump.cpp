#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>

#include <boost/asio.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/asio/steady_timer.hpp>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "InterfaceMonitor.hpp"
#include "Manager.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"

namespace {

auto FormatIpAddress(const gh::Interface::Ip4Address& addr) -> std::string {
  return boost::asio::ip::address_v4(addr.Bytes).to_string();
}

auto FormatIpAddress(const gh::Interface::Ip6Address& addr) -> std::string {
  return boost::asio::ip::address_v6(addr.Bytes).to_string();
}

void PrintInterfaces(
    const std::map<gh::windows::network::InterfaceMonitor::InterfaceKey, gh::Interface::InterfaceInfo>& interfaces) {
  std::cout << "=================== Network Interfaces ===================" << std::endl;
  if (interfaces.empty()) {
    std::cout << "No interfaces found." << std::endl;
  } else {
    for (const auto& [index, info] : interfaces) {
      std::cout << "Interface " << index << " (" << info.InterfaceName.value_or("Unnamed")
                << ") [LUID: " << info.InterfaceLuid << "]\n";
      if (info.V4Info) {
        std::cout << "  IPv4: MTU=" << info.V4Info->Common.NlMtu << ", Metric=" << info.V4Info->Common.Metric
                  << ", Connected=" << (info.V4Info->Common.Connected ? "Yes" : "No")
                  << ", Forwarding=" << (info.V4Info->Common.ForwardingEnabled ? "Yes" : "No") << "\n";
        for (const auto& addr : info.V4Info->Addresses) {
          std::cout << "    Address: " << FormatIpAddress(addr.Address) << "/" << static_cast<int>(addr.PrefixLength)
                    << "\n";
        }
        for (const auto& dns : info.V4Info->DnsServers.Servers) {
          std::cout << "    DNS (IsDhcp=" << (info.V4Info->DnsServers.IsDhcp ? "Yes" : "No")
                    << "): " << FormatIpAddress(dns) << "\n";
        }
        if (info.V4Info->OriginalDnsServers.has_value()) {
          std::cout << "    Original DNS (IsDhcp=" << (info.V4Info->OriginalDnsServers->IsDhcp ? "Yes" : "No") << "):\n";
          for (const auto& dns : info.V4Info->OriginalDnsServers->Servers) {
            std::cout << "      " << FormatIpAddress(dns) << "\n";
          }
        }
      }
      if (info.V6Info) {
        std::cout << "  IPv6: MTU=" << info.V6Info->Common.NlMtu << ", Metric=" << info.V6Info->Common.Metric
                  << ", Connected=" << (info.V6Info->Common.Connected ? "Yes" : "No")
                  << ", Forwarding=" << (info.V6Info->Common.ForwardingEnabled ? "Yes" : "No") << "\n";
        for (const auto& addr : info.V6Info->Addresses) {
          std::cout << "    Address: " << FormatIpAddress(addr.Address) << "/" << static_cast<int>(addr.PrefixLength)
                    << "\n";
        }
        for (const auto& dns : info.V6Info->DnsServers.Servers) {
          std::cout << "    DNS (IsDhcp=" << (info.V6Info->DnsServers.IsDhcp ? "Yes" : "No")
                    << "): " << FormatIpAddress(dns) << "\n";
        }
        if (info.V6Info->OriginalDnsServers.has_value()) {
          std::cout << "    Original DNS (IsDhcp=" << (info.V6Info->OriginalDnsServers->IsDhcp ? "Yes" : "No") << "):\n";
          for (const auto& dns : info.V6Info->OriginalDnsServers->Servers) {
            std::cout << "      " << FormatIpAddress(dns) << "\n";
          }
        }
      }
    }
  }
  std::cout << "==========================================================" << std::endl;
}

} // namespace

auto main(int argc, char* argv[]) -> int {
  boost::asio::io_context ioContext;
  Omni::Fiber::AsioExecutor executor(ioContext.get_executor());
  Omni::Fiber::Manager manager(executor);
  gh::Cancel cancel;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    std::shared_ptr<gh::windows::network::InterfaceMonitor> monitor;
    monitor = std::make_shared<gh::windows::network::InterfaceMonitor>(ioContext.get_executor());

    auto startErr = co_await monitor->Start();
    if (startErr) {
      std::cerr << "Failed to start InterfaceMonitor: " << startErr.message() << "\n";
      co_return;
    }

    std::cout << "Monitoring for changes and dumping interface info every 10 seconds... Press Ctrl+C to exit.\n";
    while (!cancel.IsTriggered()) {
      PrintInterfaces(monitor->GetInterfaces(true));

      boost::asio::steady_timer timer(ioContext, std::chrono::seconds(10));
      co_await Omni::Fiber::Select(
          Omni::Fiber::SelectPair(cancel.GetFiberCancelEvent(), [] -> void {}),
          Omni::Fiber::SelectPair(timer.async_wait(cancel.AsioSlot()()),
                                  Omni::Fiber::AsioApply([](auto) -> void {})));
    }

    co_await monitor->Stop();
  });

  boost::asio::signal_set signals(ioContext, SIGINT, SIGTERM);
  signals.async_wait([&](const boost::system::error_code&, int) -> void {
    std::cout << "\nStopping InterfaceMonitor...\n";
    cancel.Trigger();
  });

  ioContext.run();
  return 0;
}
