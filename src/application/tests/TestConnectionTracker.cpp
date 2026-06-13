#include <chrono>
#include <memory>
#include <vector>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "ConnectionTracker.hpp"
#include "Coroutine.hpp"
#include "EndpointUdpDynMux.hpp"
#include "Packet.hpp"

using namespace gh;

namespace {

Packet CreateIPv6Packet(const boost::asio::ip::address_v6& src, const boost::asio::ip::address_v6& dst,
                        uint16_t srcPort, uint16_t dstPort, uint8_t protocol) {
  Packet p(44, 0);
  auto data = p.Data();
  std::fill(data.begin(), data.end(), 0);
  data[0] = 0x60;     // Version 6
  data[6] = protocol; // Next Header

  auto srcBytes = src.to_bytes();
  std::copy(srcBytes.begin(), srcBytes.end(), data.begin() + 8);

  auto dstBytes = dst.to_bytes();
  std::copy(dstBytes.begin(), dstBytes.end(), data.begin() + 24);

  if (protocol == 6 || protocol == 17) {
    data[40] = (srcPort >> 8) & 0xFF;
    data[41] = srcPort & 0xFF;
    data[42] = (dstPort >> 8) & 0xFF;
    data[43] = dstPort & 0xFF;
  }

  return p;
}

} // namespace

TEST(ConnectionTrackerTest, BasicOperations) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto mux = std::make_shared<UdpDynMux>(io);
    EXPECT_FALSE(co_await mux->Start());

    UdpDynMux::PskType psk1 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    UdpDynMux::PskType psk2 = {16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};

    auto channel1 = co_await mux->CreateChannel(psk1);
    auto channel2 = co_await mux->CreateChannel(psk2);

    EXPECT_NE(channel1, nullptr);
    EXPECT_NE(channel2, nullptr);
    if (channel1 == nullptr || channel2 == nullptr) {
      co_return;
    }

    ConnectionTracker tracker(std::chrono::seconds(2));

    auto p1 = CreateIPv6Packet(boost::asio::ip::make_address_v6("fd00::1"), boost::asio::ip::make_address_v6("fd00::2"),
                               1234, 80, 6);
    auto p1_reply = CreateIPv6Packet(boost::asio::ip::make_address_v6("fd00::2"),
                                     boost::asio::ip::make_address_v6("fd00::1"), 80, 1234, 6);

    auto p2 = CreateIPv6Packet(boost::asio::ip::make_address_v6("fd00::3"), boost::asio::ip::make_address_v6("fd00::4"),
                               5678, 443, 6);

    // Initially empty
    EXPECT_EQ(tracker.Lookup(p1, ConnectionDirection::kOutput), nullptr);

    // Update/Insert p1
    tracker.Update(p1, channel1, ConnectionDirection::kOutput);
    EXPECT_EQ(tracker.Lookup(p1, ConnectionDirection::kOutput), channel1);
    EXPECT_EQ(tracker.Lookup(p1_reply, ConnectionDirection::kInput), channel1);

    // Update/Insert p2
    tracker.Update(p2, channel2, ConnectionDirection::kOutput);
    EXPECT_EQ(tracker.Lookup(p2, ConnectionDirection::kOutput), channel2);

    // Remove channel1
    tracker.RemoveChannel(channel1);
    EXPECT_EQ(tracker.Lookup(p1, ConnectionDirection::kOutput), nullptr);
    EXPECT_EQ(tracker.Lookup(p2, ConnectionDirection::kOutput), channel2);

    // Clear
    tracker.Clear();
    EXPECT_EQ(tracker.Lookup(p2, ConnectionDirection::kOutput), nullptr);

    // Stop channel and mux
    co_await mux->RemoveChannel(psk1);
    co_await mux->RemoveChannel(psk2);
    co_await mux->Stop();
    co_await mux->WaitService();

    testPassed = true;
    co_return;
  });

  io.restart();
  io.run();
  EXPECT_TRUE(testPassed);
}

TEST(ConnectionTrackerTest, ExpirationAndPruning) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto mux = std::make_shared<UdpDynMux>(io);
    EXPECT_FALSE(co_await mux->Start());

    UdpDynMux::PskType psk1 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    auto channel1 = co_await mux->CreateChannel(psk1);
    EXPECT_NE(channel1, nullptr);
    if (channel1 == nullptr) {
      co_return;
    }

    // 1-second timeout
    ConnectionTracker tracker(std::chrono::seconds(1));

    auto p1 = CreateIPv6Packet(boost::asio::ip::make_address_v6("fd00::1"), boost::asio::ip::make_address_v6("fd00::2"),
                               1234, 80, 6);

    tracker.Update(p1, channel1, ConnectionDirection::kOutput);
    EXPECT_EQ(tracker.Lookup(p1, ConnectionDirection::kOutput), channel1);

    // Wait for it to expire using a timer in fiber
    boost::asio::steady_timer waitTimer(io);
    waitTimer.expires_after(std::chrono::milliseconds(1100));
    co_await waitTimer.async_wait(Omni::Fiber::AsioUseFiber);

    // Lookup should return nullptr and prune the entry
    EXPECT_EQ(tracker.Lookup(p1, ConnectionDirection::kOutput), nullptr);

    co_await mux->RemoveChannel(psk1);
    co_await mux->Stop();
    co_await mux->WaitService();

    testPassed = true;
    co_return;
  });

  io.restart();
  io.run();
  EXPECT_TRUE(testPassed);
}

TEST(ConnectionTrackerTest, SelectorAndValidator) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto mux = std::make_shared<UdpDynMux>(io);
    EXPECT_FALSE(co_await mux->Start());

    UdpDynMux::PskType psk1 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    auto channel1 = co_await mux->CreateChannel(psk1);
    EXPECT_NE(channel1, nullptr);
    if (channel1 == nullptr) {
      co_return;
    }

    ConnectionTracker tracker(std::chrono::seconds(2));
    auto p1 = CreateIPv6Packet(boost::asio::ip::make_address_v6("fd00::1"), boost::asio::ip::make_address_v6("fd00::2"),
                               1234, 80, 6);

    // Lookup with selector
    auto channel = tracker.Lookup(p1, ConnectionDirection::kOutput, nullptr,
                                  [&](const boost::asio::ip::address_v6& src, const boost::asio::ip::address_v6& dst,
                                      uint16_t srcPort, uint16_t dstPort, uint8_t protocol) { return channel1; });
    EXPECT_EQ(channel, channel1);

    // Subsequent lookup should find it directly
    EXPECT_EQ(tracker.Lookup(p1, ConnectionDirection::kOutput), channel1);

    // Lookup with validator that returns false
    channel = tracker.Lookup(p1, ConnectionDirection::kOutput, [](const std::shared_ptr<Endpoint>&) { return false; });
    EXPECT_EQ(channel, nullptr);

    // Should be erased now
    EXPECT_EQ(tracker.Lookup(p1, ConnectionDirection::kOutput), nullptr);

    co_await mux->RemoveChannel(psk1);
    co_await mux->Stop();
    co_await mux->WaitService();

    testPassed = true;
    co_return;
  });

  io.restart();
  io.run();
  EXPECT_TRUE(testPassed);
}
