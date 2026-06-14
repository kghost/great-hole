#include <chrono>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "ConnectionTracker.hpp"
#include "Coroutine.hpp"
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

class MockConnectionMark : public ConnectionMark {
public:
  explicit MockConnectionMark(std::string name) : _Name(std::move(name)) {}
  std::string GetDescription() const override { return _Name; }

private:
  std::string _Name;
};

class MockSelector : public ConnectionTracker::Selector {
public:
  explicit MockSelector(std::optional<std::reference_wrapper<ConnectionMark>> result) : Result(result) {}

  std::optional<std::reference_wrapper<ConnectionMark>> operator()(const ConnectionTracker::Ip4TcpKey&) const override {
    return Result;
  }
  std::optional<std::reference_wrapper<ConnectionMark>> operator()(const ConnectionTracker::Ip6TcpKey&) const override {
    return Result;
  }
  std::optional<std::reference_wrapper<ConnectionMark>> operator()(const ConnectionTracker::Ip4UdpKey&) const override {
    return Result;
  }
  std::optional<std::reference_wrapper<ConnectionMark>> operator()(const ConnectionTracker::Ip6UdpKey&) const override {
    return Result;
  }
  std::optional<std::reference_wrapper<ConnectionMark>> operator()(const ConnectionTracker::IcmpKey&) const override {
    return Result;
  }
  std::optional<std::reference_wrapper<ConnectionMark>> operator()(const ConnectionTracker::Icmp6Key&) const override {
    return Result;
  }

  mutable std::optional<std::reference_wrapper<ConnectionMark>> Result;
};

} // namespace

TEST(ConnectionTrackerTest, BasicOperations) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    MockConnectionMark mark1("mark1");
    MockConnectionMark mark2("mark2");

    MockSelector selector(std::nullopt);
    ConnectionTracker tracker(selector, std::chrono::seconds(2));

    auto p1 = CreateIPv6Packet(boost::asio::ip::make_address_v6("fd00::1"), boost::asio::ip::make_address_v6("fd00::2"),
                               1234, 80, 6);
    auto p1_reply = CreateIPv6Packet(boost::asio::ip::make_address_v6("fd00::2"),
                                     boost::asio::ip::make_address_v6("fd00::1"), 80, 1234, 6);

    auto p2 = CreateIPv6Packet(boost::asio::ip::make_address_v6("fd00::3"), boost::asio::ip::make_address_v6("fd00::4"),
                               5678, 443, 6);

    // Initially empty
    EXPECT_FALSE(tracker.Lookup(p1, ConnectionDirection::kOutput).has_value());

    // Update/Insert p1
    tracker.Update(p1, mark1, ConnectionDirection::kOutput);
    auto res1 = tracker.Lookup(p1, ConnectionDirection::kOutput);
    EXPECT_TRUE(res1.has_value());
    EXPECT_EQ(&res1->get(), &mark1);

    auto res1_reply = tracker.Lookup(p1_reply, ConnectionDirection::kInput);
    EXPECT_TRUE(res1_reply.has_value());
    EXPECT_EQ(&res1_reply->get(), &mark1);

    // Update/Insert p2
    tracker.Update(p2, mark2, ConnectionDirection::kOutput);
    auto res2 = tracker.Lookup(p2, ConnectionDirection::kOutput);
    EXPECT_TRUE(res2.has_value());
    EXPECT_EQ(&res2->get(), &mark2);

    // Remove mark1
    tracker.RemoveMark(mark1);
    EXPECT_FALSE(tracker.Lookup(p1, ConnectionDirection::kOutput).has_value());
    EXPECT_TRUE(tracker.Lookup(p2, ConnectionDirection::kOutput).has_value());
    EXPECT_EQ(&tracker.Lookup(p2, ConnectionDirection::kOutput)->get(), &mark2);

    // Clear
    tracker.Clear();
    EXPECT_FALSE(tracker.Lookup(p2, ConnectionDirection::kOutput).has_value());

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
    MockConnectionMark mark1("mark1");
    MockSelector selector(std::nullopt);
    ConnectionTracker tracker(selector, std::chrono::seconds(1));

    auto p1 = CreateIPv6Packet(boost::asio::ip::make_address_v6("fd00::1"), boost::asio::ip::make_address_v6("fd00::2"),
                               1234, 80, 6);

    tracker.Update(p1, mark1, ConnectionDirection::kOutput);
    auto res = tracker.Lookup(p1, ConnectionDirection::kOutput);
    EXPECT_TRUE(res.has_value());
    EXPECT_EQ(&res->get(), &mark1);

    // Wait for it to expire using a timer in fiber
    boost::asio::steady_timer waitTimer(io);
    waitTimer.expires_after(std::chrono::milliseconds(1100));
    co_await waitTimer.async_wait(Omni::Fiber::AsioUseFiber);

    // Lookup should return std::nullopt and prune the entry
    EXPECT_FALSE(tracker.Lookup(p1, ConnectionDirection::kOutput).has_value());

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
    MockConnectionMark mark1("mark1");
    MockSelector selector(mark1);
    ConnectionTracker tracker(selector, std::chrono::seconds(2));
    auto p1 = CreateIPv6Packet(boost::asio::ip::make_address_v6("fd00::1"), boost::asio::ip::make_address_v6("fd00::2"),
                               1234, 80, 6);

    // Lookup with selector
    auto res = tracker.Lookup(p1, ConnectionDirection::kOutput);
    EXPECT_TRUE(res.has_value());
    EXPECT_EQ(&res->get(), &mark1);

    // Subsequent lookup should find it directly
    EXPECT_TRUE(tracker.Lookup(p1, ConnectionDirection::kOutput).has_value());
    EXPECT_EQ(&tracker.Lookup(p1, ConnectionDirection::kOutput)->get(), &mark1);

    // Lookup with validator that returns false
    selector.Result = std::nullopt;
    res = tracker.Lookup(p1, ConnectionDirection::kOutput, [](ConnectionMark&) { return false; });
    EXPECT_FALSE(res.has_value());

    // Should be erased now
    EXPECT_FALSE(tracker.Lookup(p1, ConnectionDirection::kOutput).has_value());

    testPassed = true;
    co_return;
  });

  io.restart();
  io.run();
  EXPECT_TRUE(testPassed);
}
