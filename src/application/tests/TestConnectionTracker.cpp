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
  explicit MockSelector(ConnectionTracker::RouteResult result) : Result(result) {}

  ConnectionTracker::RouteResult operator()(const ConnectionTracker::Ip4TcpKey&) const override { return Result; }
  ConnectionTracker::RouteResult operator()(const ConnectionTracker::Ip6TcpKey&) const override { return Result; }
  ConnectionTracker::RouteResult operator()(const ConnectionTracker::Ip4UdpKey&) const override { return Result; }
  ConnectionTracker::RouteResult operator()(const ConnectionTracker::Ip6UdpKey&) const override { return Result; }
  ConnectionTracker::RouteResult operator()(const ConnectionTracker::IcmpKey&) const override { return Result; }
  ConnectionTracker::RouteResult operator()(const ConnectionTracker::Icmp6Key&) const override { return Result; }

  mutable ConnectionTracker::RouteResult Result;
};

static ConnectionMark* GetMark(const ConnectionTracker::RouteResult& route) {
  if (std::holds_alternative<std::reference_wrapper<ConnectionMark>>(route)) {
    return &std::get<std::reference_wrapper<ConnectionMark>>(route).get();
  }
  return nullptr;
}

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

  Omni::TimeTravel::Client timeClient;
  AsioWarpListener listener(io);
  timeClient.RegisterListener(listener);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    MockConnectionMark mark1("mark1");
    MockConnectionMark mark2("mark2");

    MockSelector selector(ConnectionTracker::Discard{});
    auto tracker = std::make_shared<ConnectionTracker>(io.get_executor(), selector);
    auto errStart = co_await tracker->Start();
    EXPECT_FALSE(errStart);

    // Using real captured IPv6 TCP packets
    auto p1 = CreatePacket(test::captured::Ip6TcpSyn);
    auto p1_reply = CreatePacket(test::captured::Ip6TcpSynAck);

    // Using real captured IPv4 TCP packets
    auto p2 = CreatePacket(test::captured::Ip4TcpSyn);

    // Initially empty
    EXPECT_TRUE(std::holds_alternative<ConnectionTracker::Discard>(
        tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p1, ConnectionTracker::Nothing{},
                                                                               nullptr)));

    // Update/Insert p1
    selector.Result = mark1;
    auto res1 = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p1, ConnectionTracker::Nothing{},
                                                                                       nullptr);
    EXPECT_TRUE(std::holds_alternative<ConnectionTracker::Discard>(res1));

    timeClient.FastForward(std::chrono::seconds(61));

    auto res1m = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(
        p1, ConnectionTracker::Nothing{}, nullptr);
    EXPECT_TRUE(std::holds_alternative<std::reference_wrapper<ConnectionMark>>(res1m));
    EXPECT_EQ(GetMark(res1m), &mark1);

    tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionInput>(p1_reply, mark1, nullptr);
    auto res1_reply = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(
        p1, ConnectionTracker::Nothing{}, nullptr);
    EXPECT_TRUE(std::holds_alternative<std::reference_wrapper<ConnectionMark>>(res1_reply));
    EXPECT_EQ(GetMark(res1_reply), &mark1);

    // Update/Insert p2
    selector.Result = mark2;
    auto res2 = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p2, ConnectionTracker::Nothing{},
                                                                                       nullptr);
    EXPECT_TRUE(std::holds_alternative<std::reference_wrapper<ConnectionMark>>(res2));
    EXPECT_EQ(GetMark(res2), &mark2);

    // Remove mark1
    selector.Result = ConnectionTracker::Discard{};
    tracker->RemoveMark(mark1);
    EXPECT_TRUE(std::holds_alternative<ConnectionTracker::Discard>(
        tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p1, ConnectionTracker::Nothing{},
                                                                               nullptr)));
    EXPECT_TRUE(std::holds_alternative<std::reference_wrapper<ConnectionMark>>(
        tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p2, ConnectionTracker::Nothing{},
                                                                               nullptr)));
    EXPECT_EQ(GetMark(tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(
                  p2, ConnectionTracker::Nothing{}, nullptr)),
              &mark2);

    // Clear
    tracker->Clear();
    EXPECT_TRUE(std::holds_alternative<ConnectionTracker::Discard>(
        tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p2, ConnectionTracker::Nothing{},
                                                                               nullptr)));

    auto errStop = co_await tracker->Stop();
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
    MockSelector selector(ConnectionTracker::Discard{});
    auto tracker = std::make_shared<ConnectionTracker>(io.get_executor(), selector);

    auto errStart = co_await tracker->Start();
    EXPECT_FALSE(errStart);

    auto p1 = CreatePacket(test::captured::Ip6TcpSyn);

    selector.Result = mark1;
    auto res = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p1, ConnectionTracker::Nothing{},
                                                                                      nullptr);
    EXPECT_TRUE(std::holds_alternative<std::reference_wrapper<ConnectionMark>>(res));
    EXPECT_EQ(GetMark(res), &mark1);

    // Warp monotonic clock forward for SYN timeout (61s)
    timeClient.FastForward(std::chrono::seconds(61));

    // Lookup should return Discard and prune the entry
    selector.Result = ConnectionTracker::Discard{};
    EXPECT_TRUE(std::holds_alternative<ConnectionTracker::Discard>(
        tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p1, ConnectionTracker::Nothing{},
                                                                               nullptr)));

    auto errStop = co_await tracker->Stop();
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
    auto res = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p1, ConnectionTracker::Nothing{},
                                                                                      nullptr);
    EXPECT_TRUE(std::holds_alternative<std::reference_wrapper<ConnectionMark>>(res));
    EXPECT_EQ(GetMark(res), &mark1);

    // Subsequent lookup should find it directly
    EXPECT_TRUE(std::holds_alternative<std::reference_wrapper<ConnectionMark>>(
        tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p1, ConnectionTracker::Nothing{},
                                                                               nullptr)));
    EXPECT_EQ(GetMark(tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(
                  p1, ConnectionTracker::Nothing{}, nullptr)),
              &mark1);

    // Lookup with validator that returns false
    selector.Result = ConnectionTracker::Discard{};
    res = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p1, ConnectionTracker::Nothing{},
                                                                                 [](ConnectionMark&) { return false; });
    EXPECT_TRUE(std::holds_alternative<ConnectionTracker::Discard>(res));

    // Should be erased now
    EXPECT_TRUE(std::holds_alternative<ConnectionTracker::Discard>(
        tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p1, ConnectionTracker::Nothing{},
                                                                               nullptr)));

    auto errStop = co_await tracker->Stop();
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
    MockSelector selector(ConnectionTracker::Discard{});
    auto tracker = std::make_shared<ConnectionTracker>(io.get_executor(), selector);

    auto errStart = co_await tracker->Start();
    EXPECT_FALSE(errStart);

    // 1. SYN packet (state starts at SynSent, default timeout = 60s)
    auto pSyn = CreatePacket(test::captured::Ip6TcpSyn);
    selector.Result = mark1;
    auto res = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(
        pSyn, ConnectionTracker::Nothing{}, nullptr);
    EXPECT_TRUE(std::holds_alternative<std::reference_wrapper<ConnectionMark>>(res));
    EXPECT_EQ(GetMark(res), &mark1);

    // Warp monotonic clock forward for SYN timeout (61s)
    timeClient.FastForward(std::chrono::seconds(61));

    // Should be expired now
    selector.Result = ConnectionTracker::Discard{};
    EXPECT_TRUE(std::holds_alternative<ConnectionTracker::Discard>(
        tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(pSyn, ConnectionTracker::Nothing{},
                                                                               nullptr)));

    // 2. Re-establish, then send SYN-ACK to establish (state transitions to Established, default timeout = 1200s)
    selector.Result = mark1;
    tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(pSyn, ConnectionTracker::Nothing{}, nullptr);
    auto pSynAck = CreatePacket(test::captured::Ip6TcpSynAck);
    tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionInput>(pSynAck, mark1, nullptr);
    auto checkSynAck = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(
        pSyn, ConnectionTracker::Nothing{}, nullptr);
    EXPECT_TRUE(std::holds_alternative<std::reference_wrapper<ConnectionMark>>(checkSynAck));

    // Warp monotonic clock forward 61s again. In established state, it shouldn't expire!
    timeClient.FastForward(std::chrono::seconds(61));
    EXPECT_TRUE(std::holds_alternative<std::reference_wrapper<ConnectionMark>>(
        tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(pSyn, ConnectionTracker::Nothing{},
                                                                               nullptr)));

    // 3. Send FIN packet (state transitions to FinWait, default timeout = 30s)
    auto pFin = CreatePacket(test::captured::Ip6TcpFin);
    selector.Result = mark1;
    tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(pFin, ConnectionTracker::Nothing{}, nullptr);

    // Warp monotonic clock forward 31s. It should expire!
    timeClient.FastForward(std::chrono::seconds(31));
    selector.Result = ConnectionTracker::Discard{};
    EXPECT_TRUE(std::holds_alternative<ConnectionTracker::Discard>(
        tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(pSyn, ConnectionTracker::Nothing{},
                                                                               nullptr)));

    auto errStop = co_await tracker->Stop();
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
    MockSelector selector(ConnectionTracker::Discard{});
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
      selector.Result = mark1;
      tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(originalUdp, ConnectionTracker::Nothing{},
                                                                             nullptr);

      // Now use the real ICMPv4 Destination Unreachable packet
      auto icmpPacket = CreatePacket(test::captured::Icmp4DestUnreachable);

      // Lookup of the ICMP packet (direction: Input) should update state, and lookup output should return mark1
      tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionInput>(icmpPacket, mark1, nullptr);
      auto checkRes = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(
          originalUdp, ConnectionTracker::Nothing{}, nullptr);
      EXPECT_TRUE(std::holds_alternative<std::reference_wrapper<ConnectionMark>>(checkRes));
      EXPECT_EQ(GetMark(checkRes), &mark1);
    }

    // IPv6 association test
    {
      // Extract original inner packet from captured ICMPv6 Destination Unreachable
      std::vector<uint8_t> originalBytes(test::captured::Icmp6DestUnreachable.begin() + 48,
                                         test::captured::Icmp6DestUnreachable.end());
      auto originalUdp = CreatePacket(originalBytes);

      // Update tracker with original UDP connection
      selector.Result = mark1;
      tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(originalUdp, ConnectionTracker::Nothing{},
                                                                             nullptr);

      // Now use the real ICMPv6 Destination Unreachable packet
      auto icmpPacket = CreatePacket(test::captured::Icmp6DestUnreachable);

      // Lookup of the ICMPv6 packet should update state, and lookup output should return mark1
      tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionInput>(icmpPacket, mark1, nullptr);
      auto checkRes = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(
          originalUdp, ConnectionTracker::Nothing{}, nullptr);
      EXPECT_TRUE(std::holds_alternative<std::reference_wrapper<ConnectionMark>>(checkRes));
      EXPECT_EQ(GetMark(checkRes), &mark1);
    }

    auto errStop = co_await tracker->Stop();
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
