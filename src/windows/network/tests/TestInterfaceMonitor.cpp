#include <memory>

#include <gtest/gtest.h>

#include <boost/asio.hpp>

#include "Asio.hpp"
#include "InterfaceMonitor.hpp"
#include "Manager.hpp"

namespace gh::windows::network::tests {

class InterfaceMonitorTest : public ::testing::Test {
protected:
  boost::asio::io_context _IoContext;
};

TEST_F(InterfaceMonitorTest, StartStopLifecycle) {
  Omni::Fiber::AsioExecutor executor(_IoContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  manager.SpawnRoot("root", [this]() -> Omni::Fiber::Coroutine<void> {
    auto monitor = std::make_shared<InterfaceMonitor>(_IoContext.get_executor());

    EXPECT_EQ(monitor->GetState(), ServiceBase::State::kNone);
    auto err = co_await monitor->Start();
    EXPECT_FALSE(err);
    EXPECT_EQ(monitor->GetState(), ServiceBase::State::kRunning);

    auto stopErr = co_await monitor->Stop();
    EXPECT_FALSE(stopErr);
    EXPECT_EQ(monitor->GetState(), ServiceBase::State::kNone);
  });
  _IoContext.run();
}

TEST_F(InterfaceMonitorTest, UnicastAddressCheck) {
  Omni::Fiber::AsioExecutor executor(_IoContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  manager.SpawnRoot("root", [this]() -> Omni::Fiber::Coroutine<void> {
    auto monitor = std::make_shared<InterfaceMonitor>(_IoContext.get_executor());
    auto err = co_await monitor->Start();
    EXPECT_FALSE(err);

    // Loopback addresses are intentionally filtered out by ConvertSockaddrInet
    auto loopbackV4 = monitor->GetAddressInfo(boost::asio::ip::make_address_v4("127.0.0.1"));
    auto loopbackV6 = monitor->GetAddressInfo(boost::asio::ip::make_address_v6("::1"));

    EXPECT_FALSE(loopbackV4.has_value());
    EXPECT_FALSE(loopbackV6.has_value());

    co_await monitor->Stop();
  });
  _IoContext.run();
}

TEST_F(InterfaceMonitorTest, GetAddressInfoNonExistent) {
  Omni::Fiber::AsioExecutor executor(_IoContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  manager.SpawnRoot("root", [this]() -> Omni::Fiber::Coroutine<void> {
    auto monitor = std::make_shared<InterfaceMonitor>(_IoContext.get_executor());
    auto err = co_await monitor->Start();
    EXPECT_FALSE(err);

    auto nonExistent = monitor->GetAddressInfo(boost::asio::ip::make_address_v4("203.0.113.199"));
    EXPECT_FALSE(nonExistent.has_value());

    co_await monitor->Stop();
  });
  _IoContext.run();
}

TEST_F(InterfaceMonitorTest, IpInterfaceCheck) {
  Omni::Fiber::AsioExecutor executor(_IoContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  manager.SpawnRoot("root", [this]() -> Omni::Fiber::Coroutine<void> {
    auto monitor = std::make_shared<InterfaceMonitor>(_IoContext.get_executor());
    auto err = co_await monitor->Start();
    EXPECT_FALSE(err);

    const auto& interfaces = monitor->GetInterfaces(false);
    EXPECT_FALSE(interfaces.empty());

    bool foundAddress = false;
    for (const auto& [key, info] : interfaces) {
      EXPECT_EQ(key, info.InterfaceIndex);
      EXPECT_TRUE(info.V4Info.has_value() || info.V6Info.has_value());
      if (info.V4Info.has_value() && !info.V4Info->Addresses.empty()) {
        foundAddress = true;
        for (const auto& addr : info.V4Info->Addresses) {
          EXPECT_GT(addr.PrefixLength, 0);
          EXPECT_NE(addr.Address.Bytes, (std::array<uint8_t, 4>{127, 0, 0, 1}));
        }
        for (const auto& dns : info.V4Info->DnsServers.Servers) {
          EXPECT_NE(dns.Bytes, (std::array<uint8_t, 4>{127, 0, 0, 1}));
        }
      }
      if (info.V6Info.has_value() && !info.V6Info->Addresses.empty()) {
        foundAddress = true;
        for (const auto& addr : info.V6Info->Addresses) {
          EXPECT_GT(addr.PrefixLength, 0);
          EXPECT_NE(addr.Address.Bytes, (std::array<uint8_t, 16>{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}));
        }
        for (const auto& dns : info.V6Info->DnsServers.Servers) {
          EXPECT_NE(dns.Bytes, (std::array<uint8_t, 16>{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}));
        }
      }
    }
    EXPECT_TRUE(foundAddress);

    co_await monitor->Stop();
  });
  _IoContext.run();
}

TEST_F(InterfaceMonitorTest, OverrideDnsServers) {
  Omni::Fiber::AsioExecutor executor(_IoContext.get_executor());
  Omni::Fiber::Manager manager(executor);

  manager.SpawnRoot("root", [this]() -> Omni::Fiber::Coroutine<void> {
    auto monitor = std::make_shared<InterfaceMonitor>(_IoContext.get_executor());
    auto err = co_await monitor->Start();
    EXPECT_FALSE(err);

    const auto& interfaces = monitor->GetInterfaces(false);
    EXPECT_FALSE(interfaces.empty());

    auto targetKey = interfaces.begin()->first;
    auto initialInfo = interfaces.begin()->second;

    if (initialInfo.V4Info.has_value()) {
      EXPECT_FALSE(initialInfo.V4Info->OriginalDnsServers.has_value());
      auto initialDns = initialInfo.V4Info->DnsServers;

      std::vector<gh::Interface::Ip4Address> override1{
          gh::Interface::Ip4Address{.Bytes = {1, 1, 1, 1}},
          gh::Interface::Ip4Address{.Bytes = {1, 0, 0, 1}},
      };

      auto res1 = monitor->OverrideDnsServers(targetKey, override1);
      if (!res1) {
        // When running unit tests without administrator privileges, SetInterfaceDnsSettings returns ERROR_ACCESS_DENIED (5)
        EXPECT_NE(res1.error().find("error: 5"), std::string::npos);
        co_await monitor->Stop();
        co_return;
      }

      const auto& updatedInterfaces1 = monitor->GetInterfaces(false);
      const auto& updatedInfo1 = updatedInterfaces1.at(targetKey);
      EXPECT_TRUE(updatedInfo1.V4Info.has_value());

      if (updatedInfo1.V4Info.has_value()) {
        EXPECT_TRUE(updatedInfo1.V4Info->OriginalDnsServers.has_value());
        EXPECT_EQ(updatedInfo1.V4Info->OriginalDnsServers.value(), initialDns);
        EXPECT_EQ(updatedInfo1.V4Info->DnsServers.Servers, override1);
      }

      // Test Refresh API preserves OriginalDnsServers
      monitor->Refresh();
      const auto& refreshedInterfaces = monitor->GetInterfaces(false);
      const auto& refreshedInfo = refreshedInterfaces.at(targetKey);
      if (refreshedInfo.V4Info.has_value()) {
        EXPECT_TRUE(refreshedInfo.V4Info->OriginalDnsServers.has_value());
        EXPECT_EQ(refreshedInfo.V4Info->OriginalDnsServers.value(), initialDns);
      }

      // Second override should skip modifying OriginalDnsServers
      std::vector<gh::Interface::Ip4Address> override2{
          gh::Interface::Ip4Address{.Bytes = {8, 8, 8, 8}},
      };

      auto res2 = monitor->OverrideDnsServers(targetKey, override2);
      EXPECT_TRUE(res2.has_value());

      const auto& updatedInterfaces2 = monitor->GetInterfaces(false);
      const auto& updatedInfo2 = updatedInterfaces2.at(targetKey);
      EXPECT_TRUE(updatedInfo2.V4Info.has_value());

      if (updatedInfo2.V4Info.has_value()) {
        EXPECT_TRUE(updatedInfo2.V4Info->OriginalDnsServers.has_value());
        EXPECT_EQ(updatedInfo2.V4Info->OriginalDnsServers.value(), initialDns);
        EXPECT_EQ(updatedInfo2.V4Info->DnsServers.Servers, override2);
      }

      // Restore DNS servers
      auto resRestore = monitor->RestoreDnsServers(targetKey);
      EXPECT_TRUE(resRestore.has_value());

      const auto& restoredInterfaces = monitor->GetInterfaces(false);
      const auto& restoredInfo = restoredInterfaces.at(targetKey);
      EXPECT_TRUE(restoredInfo.V4Info.has_value());

      if (restoredInfo.V4Info.has_value()) {
        EXPECT_FALSE(restoredInfo.V4Info->OriginalDnsServers.has_value());
        EXPECT_EQ(restoredInfo.V4Info->DnsServers, initialDns);
      }
    }

    co_await monitor->Stop();
  });
  _IoContext.run();
}

} // namespace gh::windows::network::tests
