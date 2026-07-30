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

    const auto& addresses = monitor->GetAddresses();
    EXPECT_FALSE(addresses.empty());

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

    const auto& addresses = monitor->GetAddresses();
    EXPECT_FALSE(addresses.empty());

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

} // namespace gh::windows::network::tests
