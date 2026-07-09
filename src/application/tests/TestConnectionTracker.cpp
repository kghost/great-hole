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
  bool Validate() const override { return Valid; }

  bool Valid = true;

private:
  std::string _Name;
};

class TestDiscardMark : public ConnectionMark {
public:
  std::string GetDescription() const override { return "Discard"; }
};

class TestBypassMark : public ConnectionMark {
public:
  std::string GetDescription() const override { return "Bypass"; }
};

inline TestDiscardMark g_TestDiscardMark;

class ReferenceMark : public ConnectionMark {
public:
  explicit ReferenceMark(ConnectionMark& mark) : _Mark(mark) {}
  std::string GetDescription() const override { return _Mark.GetDescription(); }
  bool Validate() const override { return _Mark.Validate(); }
  ConnectionMark& GetReferencedMark() const { return _Mark; }

private:
  ConnectionMark& _Mark;
};

class MockSelector : public ConnectionTracker::Selector {
public:
  explicit MockSelector(ConnectionMark& result) : Result(&result) {}

  std::unique_ptr<ConnectionMark> operator()(const ConnectionTracker::Ip4TcpKey&) const override {
    return CloneResult();
  }
  std::unique_ptr<ConnectionMark> operator()(const ConnectionTracker::Ip6TcpKey&) const override {
    return CloneResult();
  }
  std::unique_ptr<ConnectionMark> operator()(const ConnectionTracker::Ip4UdpKey&) const override {
    return CloneResult();
  }
  std::unique_ptr<ConnectionMark> operator()(const ConnectionTracker::Ip6UdpKey&) const override {
    return CloneResult();
  }
  std::unique_ptr<ConnectionMark> operator()(const ConnectionTracker::IcmpKey&) const override { return CloneResult(); }
  std::unique_ptr<ConnectionMark> operator()(const ConnectionTracker::Icmp6Key&) const override {
    return CloneResult();
  }

  mutable ConnectionMark* Result = nullptr;

private:
  std::unique_ptr<ConnectionMark> CloneResult() const {
    if (Result) {
      return std::make_unique<ReferenceMark>(*Result);
    }
    return std::make_unique<ReferenceMark>(g_TestDiscardMark);
  }
};

class ConstantSelector : public ConnectionTracker::Selector {
public:
  explicit ConstantSelector(ConnectionMark& mark) : _Mark(mark) {}
  std::unique_ptr<ConnectionMark> operator()(const ConnectionTracker::Ip4TcpKey&) const override {
    return std::make_unique<ReferenceMark>(_Mark);
  }
  std::unique_ptr<ConnectionMark> operator()(const ConnectionTracker::Ip6TcpKey&) const override {
    return std::make_unique<ReferenceMark>(_Mark);
  }
  std::unique_ptr<ConnectionMark> operator()(const ConnectionTracker::Ip4UdpKey&) const override {
    return std::make_unique<ReferenceMark>(_Mark);
  }
  std::unique_ptr<ConnectionMark> operator()(const ConnectionTracker::Ip6UdpKey&) const override {
    return std::make_unique<ReferenceMark>(_Mark);
  }
  std::unique_ptr<ConnectionMark> operator()(const ConnectionTracker::IcmpKey&) const override {
    return std::make_unique<ReferenceMark>(_Mark);
  }
  std::unique_ptr<ConnectionMark> operator()(const ConnectionTracker::Icmp6Key&) const override {
    return std::make_unique<ReferenceMark>(_Mark);
  }

private:
  ConnectionMark& _Mark;
};

static ConnectionMark* GetMark(const std::expected<std::reference_wrapper<ConnectionMark>, ErrorCode>& res) {
  if (res.has_value()) {
    ConnectionMark& mark = res->get();
    if (auto* refMark = dynamic_cast<ReferenceMark*>(&mark)) {
      return &refMark->GetReferencedMark();
    }
    return &mark;
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

    MockSelector selector(g_TestDiscardMark);
    auto tracker = std::make_shared<ConnectionTracker>(io.get_executor());
    auto errStart = co_await tracker->Start();
    EXPECT_FALSE(errStart);

    // Using real captured IPv6 TCP packets
    auto p1 = CreatePacket(test::captured::Ip6TcpSyn);
    auto p1_reply = CreatePacket(test::captured::Ip6TcpSynAck);

    // Using real captured IPv4 TCP packets
    auto p2 = CreatePacket(test::captured::Ip4TcpSyn);

    // Initially empty
    auto resEmpty = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p1, selector);
    EXPECT_TRUE(resEmpty.has_value());
    EXPECT_EQ(GetMark(resEmpty), &g_TestDiscardMark);

    // Update/Insert p1
    selector.Result = &mark1;
    auto res1 = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p1, selector);
    EXPECT_TRUE(res1.has_value());
    EXPECT_EQ(GetMark(res1), &g_TestDiscardMark);

    timeClient.FastForward(std::chrono::seconds(61));

    auto res1m = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p1, selector);
    EXPECT_TRUE(res1m.has_value());
    EXPECT_EQ(GetMark(res1m), &mark1);

    ConstantSelector selectMark1(mark1);
    auto resInput = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionInput>(p1_reply, selectMark1);
    EXPECT_TRUE(resInput.has_value());

    auto res1_reply = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p1, selector);
    EXPECT_TRUE(res1_reply.has_value());
    EXPECT_EQ(GetMark(res1_reply), &mark1);

    // Update/Insert p2
    selector.Result = &mark2;
    auto res2 = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p2, selector);
    EXPECT_TRUE(res2.has_value());
    EXPECT_EQ(GetMark(res2), &mark2);

    selector.Result = &g_TestDiscardMark;
    tracker->Clear();
    auto resClear = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p2, selector);
    EXPECT_TRUE(resClear.has_value());
    EXPECT_EQ(GetMark(resClear), &g_TestDiscardMark);

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
    MockSelector selector(g_TestDiscardMark);
    auto tracker = std::make_shared<ConnectionTracker>(io.get_executor());

    auto errStart = co_await tracker->Start();
    EXPECT_FALSE(errStart);

    auto p1 = CreatePacket(test::captured::Ip6TcpSyn);

    selector.Result = &mark1;
    auto res = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p1, selector);
    EXPECT_TRUE(res.has_value());
    EXPECT_EQ(GetMark(res), &mark1);

    // Warp monotonic clock forward for SYN timeout (61s)
    timeClient.FastForward(std::chrono::seconds(61));

    // Lookup should return Discard and prune the entry
    selector.Result = &g_TestDiscardMark;
    auto resExpired = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p1, selector);
    EXPECT_TRUE(resExpired.has_value());
    EXPECT_EQ(GetMark(resExpired), &g_TestDiscardMark);

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
    auto tracker = std::make_shared<ConnectionTracker>(io.get_executor());
    auto errStart = co_await tracker->Start();
    EXPECT_FALSE(errStart);

    auto p1 = CreatePacket(test::captured::Ip6TcpSyn);

    // Lookup with selector
    auto res = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p1, selector);
    EXPECT_TRUE(res.has_value());
    EXPECT_EQ(GetMark(res), &mark1);

    // Subsequent lookup should find it directly
    auto resSubsequent = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p1, selector);
    EXPECT_TRUE(resSubsequent.has_value());
    EXPECT_EQ(GetMark(resSubsequent), &mark1);

    // Lookup with validation failing
    mark1.Valid = false;
    selector.Result = &g_TestDiscardMark;
    auto resInvalid = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p1, selector);
    EXPECT_TRUE(resInvalid.has_value());
    EXPECT_EQ(GetMark(resInvalid), &g_TestDiscardMark);

    // Should be erased/replaced now
    auto resErased = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(p1, selector);
    EXPECT_TRUE(resErased.has_value());
    EXPECT_EQ(GetMark(resErased), &g_TestDiscardMark);

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
    MockSelector selector(g_TestDiscardMark);
    auto tracker = std::make_shared<ConnectionTracker>(io.get_executor());

    auto errStart = co_await tracker->Start();
    EXPECT_FALSE(errStart);

    // 1. SYN packet (state starts at SynSent, default timeout = 60s)
    auto pSyn = CreatePacket(test::captured::Ip6TcpSyn);
    selector.Result = &mark1;
    auto res = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(pSyn, selector);
    EXPECT_TRUE(res.has_value());
    EXPECT_EQ(GetMark(res), &mark1);

    // Warp monotonic clock forward for SYN timeout (61s)
    timeClient.FastForward(std::chrono::seconds(61));

    // Should be expired now
    selector.Result = &g_TestDiscardMark;
    auto resExpired = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(pSyn, selector);
    EXPECT_TRUE(resExpired.has_value());
    EXPECT_EQ(GetMark(resExpired), &g_TestDiscardMark);

    // Warp monotonic clock forward to expire the discard mark entry (61s)
    timeClient.FastForward(std::chrono::seconds(61));

    // 2. Re-establish, then send SYN-ACK to establish (state transitions to Established, default timeout = 1200s)
    selector.Result = &mark1;
    auto resReestablish = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(pSyn, selector);
    EXPECT_TRUE(resReestablish.has_value());
    auto pSynAck = CreatePacket(test::captured::Ip6TcpSynAck);
    ConstantSelector selectMark1(mark1);
    auto resSynAck = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionInput>(pSynAck, selectMark1);
    EXPECT_TRUE(resSynAck.has_value());
    auto checkSynAck = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(pSyn, selector);
    EXPECT_TRUE(checkSynAck.has_value());
    EXPECT_EQ(GetMark(checkSynAck), &mark1);

    // Warp monotonic clock forward 61s again. In established state, it shouldn't expire!
    timeClient.FastForward(std::chrono::seconds(61));
    auto checkEstablished = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(pSyn, selector);
    EXPECT_TRUE(checkEstablished.has_value());
    EXPECT_EQ(GetMark(checkEstablished), &mark1);

    // 3. Send FIN packet (state transitions to FinWait, default timeout = 30s)
    auto pFin = CreatePacket(test::captured::Ip6TcpFin);
    selector.Result = &mark1;
    auto resFin = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(pFin, selector);
    EXPECT_TRUE(resFin.has_value());

    // Warp monotonic clock forward 31s. It should expire!
    timeClient.FastForward(std::chrono::seconds(31));
    selector.Result = &g_TestDiscardMark;
    auto checkFinExpired = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(pSyn, selector);
    EXPECT_TRUE(checkFinExpired.has_value());
    EXPECT_EQ(GetMark(checkFinExpired), &g_TestDiscardMark);

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
    MockSelector selector(g_TestDiscardMark);
    auto tracker = std::make_shared<ConnectionTracker>(io.get_executor());
    auto errStart = co_await tracker->Start();
    EXPECT_FALSE(errStart);

    ConstantSelector selectMark1(mark1);

    // IPv4 association test
    {
      // Extract original inner packet from captured ICMPv4 Destination Unreachable
      std::vector<uint8_t> originalBytes(test::captured::Icmp4DestUnreachable.begin() + 28,
                                         test::captured::Icmp4DestUnreachable.end());
      auto originalUdp = CreatePacket(originalBytes);

      // Update tracker with original UDP connection
      selector.Result = &mark1;
      auto resOrig = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(originalUdp, selector);
      EXPECT_TRUE(resOrig.has_value());

      // Now use the real ICMPv4 Destination Unreachable packet
      auto icmpPacket = CreatePacket(test::captured::Icmp4DestUnreachable);

      // Lookup of the ICMP packet (direction: Input) should update state, and lookup output should return mark1
      auto resIcmp = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionInput>(icmpPacket, selectMark1);
      EXPECT_TRUE(resIcmp.has_value());
      auto checkRes = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(originalUdp, selector);
      EXPECT_TRUE(checkRes.has_value());
      EXPECT_EQ(GetMark(checkRes), &mark1);
    }

    // IPv6 association test
    {
      // Extract original inner packet from captured ICMPv6 Destination Unreachable
      std::vector<uint8_t> originalBytes(test::captured::Icmp6DestUnreachable.begin() + 48,
                                         test::captured::Icmp6DestUnreachable.end());
      auto originalUdp = CreatePacket(originalBytes);

      // Update tracker with original UDP connection
      selector.Result = &mark1;
      auto resOrig = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(originalUdp, selector);
      EXPECT_TRUE(resOrig.has_value());

      // Now use the real ICMPv6 Destination Unreachable packet
      auto icmpPacket = CreatePacket(test::captured::Icmp6DestUnreachable);

      // Lookup of the ICMPv6 packet should update state, and lookup output should return mark1
      auto resIcmp = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionInput>(icmpPacket, selectMark1);
      EXPECT_TRUE(resIcmp.has_value());
      auto checkRes = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(originalUdp, selector);
      EXPECT_TRUE(checkRes.has_value());
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
