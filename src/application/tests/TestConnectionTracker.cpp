#include <chrono>
#include <cstdlib>
#include <iostream>
#include <vector>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "ConnectionTracker.hpp"
#include "Coroutine.hpp"
#include "Packet.hpp"
#include "TimeTravel.hpp"

#include "CapturedPackets.hpp"

using namespace gh;

namespace {

class AsioWarpListener : public Omni::TimeTravel::IWarpListener {
public:
  explicit AsioWarpListener(boost::asio::io_context& io) : _Io(io) {}

  void OnPreWarp() override {
#ifndef _WIN32
    _Io.notify_fork(boost::asio::io_context::fork_prepare);
#endif
  }

  void OnPostWarpParent() override {
#ifndef _WIN32
    _Io.notify_fork(boost::asio::io_context::fork_parent);
#endif
  }

  void OnPostWarpChild() override {
#ifndef _WIN32
    _Io.notify_fork(boost::asio::io_context::fork_child);
#endif
  }

private:
  boost::asio::io_context& _Io;
};

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

Packet CreatePacket(const std::vector<uint8_t>& bytes) {
  Packet p(bytes.size(), 0);
  std::copy(bytes.begin(), bytes.end(), p.Data().begin());
  return p;
}

} // namespace

TEST(ConnectionTrackerTest, BasicOperations) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    MockConnectionMark mark1("mark1");
    MockConnectionMark mark2("mark2");

    MockSelector selector(std::nullopt);
    auto tracker = std::make_shared<ConnectionTracker>(io.get_executor(), selector);
    auto errStart = co_await tracker->Start();
    EXPECT_FALSE(errStart);

    // Using real captured IPv6 TCP packets
    auto p1 = CreatePacket(test::captured::Ip6TcpSyn);
    auto p1_reply = CreatePacket(test::captured::Ip6TcpSynAck);

    // Using real captured IPv4 TCP packets
    auto p2 = CreatePacket(test::captured::Ip4TcpSyn);

    // Initially empty
    EXPECT_FALSE(tracker->LookupAndUpdate<ConnectionDirection::kOutput>(p1).has_value());

    // Update/Insert p1
    auto res1 = tracker->LookupAndUpdate<ConnectionDirection::kOutput>(p1, mark1);
    EXPECT_TRUE(res1.has_value());
    EXPECT_EQ(&res1->get(), &mark1);

    auto res1_reply = tracker->LookupAndUpdate<ConnectionDirection::kInput>(p1_reply);
    EXPECT_TRUE(res1_reply.has_value());
    EXPECT_EQ(&res1_reply->get(), &mark1);

    // Update/Insert p2
    auto res2 = tracker->LookupAndUpdate<ConnectionDirection::kOutput>(p2, mark2);
    EXPECT_TRUE(res2.has_value());
    EXPECT_EQ(&res2->get(), &mark2);

    // Remove mark1
    tracker->RemoveMark(mark1);
    EXPECT_FALSE(tracker->LookupAndUpdate<ConnectionDirection::kOutput>(p1).has_value());
    EXPECT_TRUE(tracker->LookupAndUpdate<ConnectionDirection::kOutput>(p2).has_value());
    EXPECT_EQ(&tracker->LookupAndUpdate<ConnectionDirection::kOutput>(p2)->get(), &mark2);

    // Clear
    tracker->Clear();
    EXPECT_FALSE(tracker->LookupAndUpdate<ConnectionDirection::kOutput>(p2).has_value());

    co_await tracker->Stop();
    auto errStop = co_await tracker->WaitService();
    EXPECT_FALSE(errStop);

    testPassed = true;
    co_return;
  });

  io.restart();
  io.run();
  EXPECT_TRUE(testPassed);
}

TEST(ConnectionTrackerTest, ExpirationAndPruning) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  Omni::TimeTravel::Client timeClient;
  AsioWarpListener listener(io);
  timeClient.RegisterListener(listener);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    MockConnectionMark mark1("mark1");
    MockSelector selector(std::nullopt);
    auto tracker = std::make_shared<ConnectionTracker>(io.get_executor(), selector);

    auto errStart = co_await tracker->Start();
    EXPECT_FALSE(errStart);

    auto p1 = CreatePacket(test::captured::Ip6TcpSyn);

    auto res = tracker->LookupAndUpdate<ConnectionDirection::kOutput>(p1, mark1);
    EXPECT_TRUE(res.has_value());
    EXPECT_EQ(&res->get(), &mark1);

    // Warp monotonic clock forward for SYN timeout (61s)
    timeClient.FastForward(std::chrono::seconds(61));

    // Lookup should return std::nullopt and prune the entry
    EXPECT_FALSE(tracker->LookupAndUpdate<ConnectionDirection::kOutput>(p1).has_value());

    co_await tracker->Stop();
    auto errStop = co_await tracker->WaitService();
    EXPECT_FALSE(errStop);

    testPassed = true;
    co_return;
  });

  io.restart();
  io.run();
  EXPECT_TRUE(testPassed);
}

TEST(ConnectionTrackerTest, SelectorAndValidator) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    MockConnectionMark mark1("mark1");
    MockSelector selector(mark1);
    auto tracker = std::make_shared<ConnectionTracker>(io.get_executor(), selector);
    auto errStart = co_await tracker->Start();
    EXPECT_FALSE(errStart);

    auto p1 = CreatePacket(test::captured::Ip6TcpSyn);

    // Lookup with selector
    auto res = tracker->LookupAndUpdate<ConnectionDirection::kOutput>(p1);
    EXPECT_TRUE(res.has_value());
    EXPECT_EQ(&res->get(), &mark1);

    // Subsequent lookup should find it directly
    EXPECT_TRUE(tracker->LookupAndUpdate<ConnectionDirection::kOutput>(p1).has_value());
    EXPECT_EQ(&tracker->LookupAndUpdate<ConnectionDirection::kOutput>(p1)->get(), &mark1);

    // Lookup with validator that returns false
    selector.Result = std::nullopt;
    res =
        tracker->LookupAndUpdate<ConnectionDirection::kOutput>(p1, std::nullopt, [](ConnectionMark&) { return false; });
    EXPECT_FALSE(res.has_value());

    // Should be erased now
    EXPECT_FALSE(tracker->LookupAndUpdate<ConnectionDirection::kOutput>(p1).has_value());

    co_await tracker->Stop();
    auto errStop = co_await tracker->WaitService();
    EXPECT_FALSE(errStop);

    testPassed = true;
    co_return;
  });

  io.restart();
  io.run();
  EXPECT_TRUE(testPassed);
}

TEST(ConnectionTrackerTest, TcpStateTransitions) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  Omni::TimeTravel::Client timeClient;
  AsioWarpListener listener(io);
  timeClient.RegisterListener(listener);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    MockConnectionMark mark1("mark1");
    MockSelector selector(std::nullopt);
    auto tracker = std::make_shared<ConnectionTracker>(io.get_executor(), selector);

    auto errStart = co_await tracker->Start();
    EXPECT_FALSE(errStart);

    // 1. SYN packet (state starts at SynSent, default timeout = 60s)
    auto pSyn = CreatePacket(test::captured::Ip6TcpSyn);
    auto res = tracker->LookupAndUpdate<ConnectionDirection::kOutput>(pSyn, mark1);
    EXPECT_TRUE(res.has_value());
    EXPECT_EQ(&res->get(), &mark1);

    // Warp monotonic clock forward for SYN timeout (61s)
    timeClient.FastForward(std::chrono::seconds(61));

    // Should be expired now
    EXPECT_FALSE(tracker->LookupAndUpdate<ConnectionDirection::kOutput>(pSyn).has_value());

    // 2. Re-establish, then send SYN-ACK to establish (state transitions to Established, default timeout = 1200s)
    tracker->LookupAndUpdate<ConnectionDirection::kOutput>(pSyn, mark1);
    auto pSynAck = CreatePacket(test::captured::Ip6TcpSynAck);
    res = tracker->LookupAndUpdate<ConnectionDirection::kInput>(pSynAck);
    EXPECT_TRUE(res.has_value());

    // Warp monotonic clock forward 61s again. In established state, it shouldn't expire!
    timeClient.FastForward(std::chrono::seconds(61));
    EXPECT_TRUE(tracker->LookupAndUpdate<ConnectionDirection::kOutput>(pSyn).has_value());

    // 3. Send FIN packet (state transitions to FinWait, default timeout = 30s)
    auto pFin = CreatePacket(test::captured::Ip6TcpFin);
    tracker->LookupAndUpdate<ConnectionDirection::kOutput>(pFin, mark1);

    // Warp monotonic clock forward 31s. It should expire!
    timeClient.FastForward(std::chrono::seconds(31));
    EXPECT_FALSE(tracker->LookupAndUpdate<ConnectionDirection::kOutput>(pSyn).has_value());

    co_await tracker->Stop();
    auto errStop = co_await tracker->WaitService();
    EXPECT_FALSE(errStop);

    testPassed = true;
    co_return;
  });

  io.restart();
  io.run();
  EXPECT_TRUE(testPassed);
}

TEST(ConnectionTrackerTest, IcmpDestinationUnreachable) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    MockConnectionMark mark1("mark1");
    MockSelector selector(std::nullopt);
    auto tracker = std::make_shared<ConnectionTracker>(io.get_executor(), selector);
    auto errStart = co_await tracker->Start();
    EXPECT_FALSE(errStart);

    // IPv4 association test
    {
      // Extract original inner packet from captured ICMPv4 Destination Unreachable
      std::vector<uint8_t> originalBytes(test::captured::Icmp4DestUnreachable.begin() + 28,
                                         test::captured::Icmp4DestUnreachable.end());
      auto originalUdp = CreatePacket(originalBytes);

      // Update tracker with original UDP connection
      tracker->LookupAndUpdate<ConnectionDirection::kOutput>(originalUdp, mark1);

      // Now use the real ICMPv4 Destination Unreachable packet
      auto icmpPacket = CreatePacket(test::captured::Icmp4DestUnreachable);

      // Lookup of the ICMP packet (direction: Input) should resolve the original UDP connection mark!
      auto res = tracker->LookupAndUpdate<ConnectionDirection::kInput>(icmpPacket);
      EXPECT_TRUE(res.has_value());
      EXPECT_EQ(&res->get(), &mark1);
    }

    // IPv6 association test
    {
      // Extract original inner packet from captured ICMPv6 Destination Unreachable
      std::vector<uint8_t> originalBytes(test::captured::Icmp6DestUnreachable.begin() + 48,
                                         test::captured::Icmp6DestUnreachable.end());
      auto originalUdp = CreatePacket(originalBytes);

      // Update tracker with original UDP connection
      tracker->LookupAndUpdate<ConnectionDirection::kOutput>(originalUdp, mark1);

      // Now use the real ICMPv6 Destination Unreachable packet
      auto icmpPacket = CreatePacket(test::captured::Icmp6DestUnreachable);

      // Lookup of the ICMPv6 packet should resolve the original UDP connection mark!
      auto res = tracker->LookupAndUpdate<ConnectionDirection::kInput>(icmpPacket);
      EXPECT_TRUE(res.has_value());
      EXPECT_EQ(&res->get(), &mark1);
    }

    co_await tracker->Stop();
    auto errStop = co_await tracker->WaitService();
    EXPECT_FALSE(errStop);

    testPassed = true;
    co_return;
  });

  io.restart();
  io.run();
  EXPECT_TRUE(testPassed);
}

int main(int argc, char* argv[]) {
  if (std::getenv("OMNI_TIMETRAVEL_IS_CHILD")) {
    std::cout << "[Child] Running Google Test suite..." << std::endl;
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
  }

  std::cout << "[Parent] Starting orchestrator..." << std::endl;
  Omni::TimeTravel::Orchestrator orchestrator;
  int status = orchestrator.Run(argv);
  std::cout << "[Parent] Orchestrator completed. Child status: " << status << std::endl;
  return status;
}
