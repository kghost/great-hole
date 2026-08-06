#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>

#include "Asio.hpp"
#include "InterfaceMonitor.hpp"
#include "Manager.hpp"

namespace {

auto FormatIpAddress(const gh::Interface::Ip4Address& addr) -> std::string {
  return boost::asio::ip::address_v4(addr.Bytes).to_string();
}

auto FormatIpAddress(const gh::Interface::Ip6Address& addr) -> std::string {
  return boost::asio::ip::address_v6(addr.Bytes).to_string();
}

void PrintInterfaceState(
    const std::string& stepName,
    const std::map<gh::windows::network::InterfaceMonitor::InterfaceKey, gh::Interface::InterfaceInfo>& interfaces,
    gh::windows::network::InterfaceMonitor::InterfaceKey targetIndex) {
  std::cout << "--- " << stepName << " ---" << std::endl;
  auto iterator = interfaces.find(targetIndex);
  if (iterator == interfaces.end()) {
    std::cout << "Target interface " << targetIndex << " not found in snapshot." << std::endl;
    return;
  }

  const auto& info = iterator->second;
  std::cout << "Interface " << info.InterfaceIndex << " (" << info.InterfaceName.value_or("Unnamed") << ")\n";
  if (info.V4Info) {
    std::cout << "  IPv4: MTU=" << info.V4Info->Common.NlMtu << ", Metric=" << info.V4Info->Common.Metric
              << ", Connected=" << (info.V4Info->Common.Connected ? "Yes" : "No")
              << ", DHCP=" << (info.V4Info->DnsServers.IsDhcp ? "Yes" : "No") << "\n";
    for (const auto& dns : info.V4Info->DnsServers.Servers) {
      std::cout << "    Active DNS: " << FormatIpAddress(dns) << "\n";
    }
    if (info.V4Info->OriginalDnsServers.has_value()) {
      std::cout << "    Original DNS (IsDhcp=" << (info.V4Info->OriginalDnsServers->IsDhcp ? "true" : "false")
                << "):\n";
      for (const auto& dns : info.V4Info->OriginalDnsServers->Servers) {
        std::cout << "      " << FormatIpAddress(dns) << "\n";
      }
    } else {
      std::cout << "    Original DNS: (none / un-overridden)\n";
    }
  }
  if (info.V6Info) {
    std::cout << "  IPv6: MTU=" << info.V6Info->Common.NlMtu << ", Metric=" << info.V6Info->Common.Metric
              << ", Connected=" << (info.V6Info->Common.Connected ? "Yes" : "No")
              << ", DHCP=" << (info.V6Info->DnsServers.IsDhcp ? "Yes" : "No") << "\n";
    for (const auto& dns : info.V6Info->DnsServers.Servers) {
      std::cout << "    Active DNS: " << FormatIpAddress(dns) << "\n";
    }
    if (info.V6Info->OriginalDnsServers.has_value()) {
      std::cout << "    Original DNS (IsDhcp=" << (info.V6Info->OriginalDnsServers->IsDhcp ? "true" : "false")
                << "):\n";
      for (const auto& dns : info.V6Info->OriginalDnsServers->Servers) {
        std::cout << "      " << FormatIpAddress(dns) << "\n";
      }
    } else {
      std::cout << "    Original DNS: (none / un-overridden)\n";
    }
  }
}

} // namespace

auto main(int argc, char* argv[]) -> int {
  boost::asio::io_context ioContext;
  Omni::Fiber::AsioExecutor executor(ioContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  uint32_t selectedIndex = 0;
  if (argc > 1) {
    selectedIndex = static_cast<uint32_t>(std::stoul(argv[1]));
  }

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto monitor = std::make_shared<gh::windows::network::InterfaceMonitor>(ioContext.get_executor());

    auto startErr = co_await monitor->Start();
    if (startErr) {
      std::cerr << "Failed to start InterfaceMonitor: " << startErr.message() << "\n";
      co_return;
    }

    const auto& interfaces = monitor->GetInterfaces(true);
    if (interfaces.empty()) {
      std::cout << "No network interfaces found." << std::endl;
      co_await monitor->Stop();
      co_return;
    }

    if (selectedIndex == 0) {
      for (const auto& [key, info] : interfaces) {
        if (info.V4Info && info.V4Info->Common.Connected) {
          selectedIndex = key;
          break;
        }
      }
      if (selectedIndex == 0) {
        selectedIndex = interfaces.begin()->first;
      }
    }

    std::cout << "=== DNS Verification Cycle for Interface Index " << selectedIndex << " ===" << std::endl;

    // Step 1: Initial Read
    PrintInterfaceState("1. Initial Read", monitor->GetInterfaces(true), selectedIndex);

    // Step 2: Override DNS Servers
    std::vector<gh::Interface::Ip4Address> testDns{
        gh::Interface::Ip4Address{.Bytes = {1, 1, 1, 1}},
        gh::Interface::Ip4Address{.Bytes = {1, 0, 0, 1}},
    };
    std::cout << "\nExecuting OverrideDnsServers to 1.1.1.1, 1.0.0.1..." << std::endl;
    auto overrideRes = monitor->OverrideDnsServers(selectedIndex, testDns);
    if (overrideRes) {
      std::cout << "OverrideDnsServers succeeded." << std::endl;
    } else {
      std::cout << "OverrideDnsServers failed: " << overrideRes.error() << std::endl;
    }

    // Step 3: Read after Override
    PrintInterfaceState("2. Read After Override", monitor->GetInterfaces(true), selectedIndex);

    // Step 4: Restore DNS Servers
    std::cout << "\nExecuting RestoreDnsServers..." << std::endl;
    auto restoreRes = monitor->RestoreDnsServers(selectedIndex);
    if (restoreRes) {
      std::cout << "RestoreDnsServers succeeded." << std::endl;
    } else {
      std::cout << "RestoreDnsServers failed: " << restoreRes.error() << std::endl;
    }

    // Step 5: Read after Restore
    PrintInterfaceState("3. Read After Restore", monitor->GetInterfaces(true), selectedIndex);

    std::cout << "\n=== DNS Verification Cycle Complete ===" << std::endl;
    co_await monitor->Stop();
  });

  ioContext.run();
  return 0;
}
