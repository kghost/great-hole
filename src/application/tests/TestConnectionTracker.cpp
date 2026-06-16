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
                        uint16_t srcPort, uint16_t dstPort, uint8_t protocol, uint8_t tcpFlags = 0) {
  Packet p(54, 0);
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
    if (protocol == 6) {
      data[53] = tcpFlags;
    }
  }

  return p;
}

Packet CreateIPv4Packet(const boost::asio::ip::address_v4& src, const boost::asio::ip::address_v4& dst,
                        uint16_t srcPort, uint16_t dstPort, uint8_t protocol, uint8_t tcpFlags = 0) {
  Packet p(40, 0);
  auto data = p.Data();
  std::fill(data.begin(), data.end(), 0);
  data[0] = 0x45; // Version 4, IHL 5
  data[9] = protocol;

  auto srcBytes = src.to_bytes();
  std::copy(srcBytes.begin(), srcBytes.end(), data.begin() + 12);

  auto dstBytes = dst.to_bytes();
  std::copy(dstBytes.begin(), dstBytes.end(), data.begin() + 16);

  if (protocol == 6 || protocol == 17) {
    data[20] = (srcPort >> 8) & 0xFF;
    data[21] = srcPort & 0xFF;
    data[22] = (dstPort >> 8) & 0xFF;
    data[23] = dstPort & 0xFF;
    if (protocol == 6) {
      data[33] = tcpFlags;
    }
  }

  return p;
}

Packet CreateIcmp4DestUnreachable(const boost::asio::ip::address_v4& src, const boost::asio::ip::address_v4& dst,
                                  const Packet& originalPacket) {
  size_t originalSize = originalPacket.DataSize();
  size_t totalSize = 20 + 8 + originalSize;
  Packet p(totalSize, 0);
  auto data = p.Data();
  std::fill(data.begin(), data.end(), 0);

  data[0] = 0x45; // Version 4, IHL 5
  data[9] = 1;    // ICMP

  auto srcBytes = src.to_bytes();
  std::copy(srcBytes.begin(), srcBytes.end(), data.begin() + 12);

  auto dstBytes = dst.to_bytes();
  std::copy(dstBytes.begin(), dstBytes.end(), data.begin() + 16);

  data[20] = 3; // Type: Destination Unreachable
  data[21] = 1; // Code: Host Unreachable

  std::copy_n(originalPacket.Data().data(), originalSize, data.data() + 28);
  return p;
}

Packet CreateIcmp6DestUnreachable(const boost::asio::ip::address_v6& src, const boost::asio::ip::address_v6& dst,
                                  const Packet& originalPacket) {
  size_t originalSize = originalPacket.DataSize();
  size_t totalSize = 40 + 8 + originalSize;
  Packet p(totalSize, 0);
  auto data = p.Data();
  std::fill(data.begin(), data.end(), 0);

  data[0] = 0x60; // Version 6
  data[6] = 58;   // ICMPv6

  auto srcBytes = src.to_bytes();
  std::copy(srcBytes.begin(), srcBytes.end(), data.begin() + 8);

  auto dstBytes = dst.to_bytes();
  std::copy(dstBytes.begin(), dstBytes.end(), data.begin() + 24);

  data[40] = 1; // Type: Destination Unreachable
  data[41] = 1; // Code

  std::copy_n(originalPacket.Data().data(), originalSize, data.data() + 48);
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

    auto p1 = CreateIPv6Packet(boost::asio::ip::make_address_v6("fd00::1"), boost::asio::ip::make_address_v6("fd00::2"),
                               1234, 80, 6);
    auto p1_reply = CreateIPv6Packet(boost::asio::ip::make_address_v6("fd00::2"),
                                     boost::asio::ip::make_address_v6("fd00::1"), 80, 1234, 6);

    auto p2 = CreateIPv6Packet(boost::asio::ip::make_address_v6("fd00::3"), boost::asio::ip::make_address_v6("fd00::4"),
                               5678, 443, 6);

    // Initially empty
    EXPECT_FALSE(tracker->Lookup(p1, ConnectionDirection::kOutput).has_value());

    // Update/Insert p1
    tracker->Update(p1, mark1, ConnectionDirection::kOutput);
    auto res1 = tracker->Lookup(p1, ConnectionDirection::kOutput);
    EXPECT_TRUE(res1.has_value());
    EXPECT_EQ(&res1->get(), &mark1);

    auto res1_reply = tracker->Lookup(p1_reply, ConnectionDirection::kInput);
    EXPECT_TRUE(res1_reply.has_value());
    EXPECT_EQ(&res1_reply->get(), &mark1);

    // Update/Insert p2
    tracker->Update(p2, mark2, ConnectionDirection::kOutput);
    auto res2 = tracker->Lookup(p2, ConnectionDirection::kOutput);
    EXPECT_TRUE(res2.has_value());
    EXPECT_EQ(&res2->get(), &mark2);

    // Remove mark1
    tracker->RemoveMark(mark1);
    EXPECT_FALSE(tracker->Lookup(p1, ConnectionDirection::kOutput).has_value());
    EXPECT_TRUE(tracker->Lookup(p2, ConnectionDirection::kOutput).has_value());
    EXPECT_EQ(&tracker->Lookup(p2, ConnectionDirection::kOutput)->get(), &mark2);

    // Clear
    tracker->Clear();
    EXPECT_FALSE(tracker->Lookup(p2, ConnectionDirection::kOutput).has_value());

    auto errStop = co_await tracker->Stop();
    EXPECT_FALSE(errStop);
    co_await tracker->WaitService();

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

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    MockConnectionMark mark1("mark1");
    MockSelector selector(std::nullopt);
    auto tracker = std::make_shared<ConnectionTracker>(io.get_executor(), selector);

    ConnectionTracker::TcpEntry::SynTimeout = std::chrono::seconds(1);

    auto errStart = co_await tracker->Start();
    EXPECT_FALSE(errStart);

    auto p1 = CreateIPv6Packet(boost::asio::ip::make_address_v6("fd00::1"), boost::asio::ip::make_address_v6("fd00::2"),
                               1234, 80, 6);

    tracker->Update(p1, mark1, ConnectionDirection::kOutput);
    auto res = tracker->Lookup(p1, ConnectionDirection::kOutput);
    EXPECT_TRUE(res.has_value());
    EXPECT_EQ(&res->get(), &mark1);

    // Wait for it to expire using a timer in fiber
    boost::asio::steady_timer waitTimer(io);
    waitTimer.expires_after(std::chrono::milliseconds(1100));
    co_await waitTimer.async_wait(Omni::Fiber::AsioUseFiber);

    // Lookup should return std::nullopt and prune the entry
    EXPECT_FALSE(tracker->Lookup(p1, ConnectionDirection::kOutput).has_value());

    auto errStop = co_await tracker->Stop();
    EXPECT_FALSE(errStop);
    co_await tracker->WaitService();

    ConnectionTracker::TcpEntry::SynTimeout = std::chrono::seconds(60);

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

    auto p1 = CreateIPv6Packet(boost::asio::ip::make_address_v6("fd00::1"), boost::asio::ip::make_address_v6("fd00::2"),
                               1234, 80, 6);

    // Lookup with selector
    auto res = tracker->Lookup(p1, ConnectionDirection::kOutput);
    EXPECT_TRUE(res.has_value());
    EXPECT_EQ(&res->get(), &mark1);

    // Subsequent lookup should find it directly
    EXPECT_TRUE(tracker->Lookup(p1, ConnectionDirection::kOutput).has_value());
    EXPECT_EQ(&tracker->Lookup(p1, ConnectionDirection::kOutput)->get(), &mark1);

    // Lookup with validator that returns false
    selector.Result = std::nullopt;
    res = tracker->Lookup(p1, ConnectionDirection::kOutput, [](ConnectionMark&) { return false; });
    EXPECT_FALSE(res.has_value());

    // Should be erased now
    EXPECT_FALSE(tracker->Lookup(p1, ConnectionDirection::kOutput).has_value());

    auto errStop = co_await tracker->Stop();
    EXPECT_FALSE(errStop);
    co_await tracker->WaitService();

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

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    MockConnectionMark mark1("mark1");
    MockSelector selector(std::nullopt);
    auto tracker = std::make_shared<ConnectionTracker>(io.get_executor(), selector);

    // Override timeouts specifically to verify different rates of expiration
    ConnectionTracker::TcpEntry::SynTimeout = std::chrono::seconds(2);
    ConnectionTracker::TcpEntry::EstablishedTimeout = std::chrono::seconds(10);
    ConnectionTracker::TcpEntry::FinTimeout = std::chrono::seconds(4);

    auto errStart = co_await tracker->Start();
    EXPECT_FALSE(errStart);

    auto clientIp = boost::asio::ip::make_address_v6("fd00::1");
    auto serverIp = boost::asio::ip::make_address_v6("fd00::2");

    // 1. SYN packet (state starts at SynSent, timeout = 2s)
    auto pSyn = CreateIPv6Packet(clientIp, serverIp, 1234, 80, 6, 0x02); // SYN
    tracker->Update(pSyn, mark1, ConnectionDirection::kOutput);

    auto res = tracker->Lookup(pSyn, ConnectionDirection::kOutput);
    EXPECT_TRUE(res.has_value());
    EXPECT_EQ(&res->get(), &mark1);

    // Wait for SYN timeout (2.1s)
    boost::asio::steady_timer waitTimer(io);
    waitTimer.expires_after(std::chrono::milliseconds(2100));
    co_await waitTimer.async_wait(Omni::Fiber::AsioUseFiber);

    // Should be expired now
    EXPECT_FALSE(tracker->Lookup(pSyn, ConnectionDirection::kOutput).has_value());

    // 2. Re-establish, then send SYN-ACK to establish (state transitions to Established, timeout = 10s)
    tracker->Update(pSyn, mark1, ConnectionDirection::kOutput);
    auto pSynAck = CreateIPv6Packet(serverIp, clientIp, 80, 1234, 6, 0x12); // SYN-ACK
    res = tracker->Lookup(pSynAck, ConnectionDirection::kInput);
    EXPECT_TRUE(res.has_value());

    // Wait 2.1s again. In established state, it shouldn't expire!
    waitTimer.expires_after(std::chrono::milliseconds(2100));
    co_await waitTimer.async_wait(Omni::Fiber::AsioUseFiber);
    EXPECT_TRUE(tracker->Lookup(pSyn, ConnectionDirection::kOutput).has_value());

    // 3. Send FIN packet (state transitions to FinWait, timeout = 4s)
    auto pFin = CreateIPv6Packet(clientIp, serverIp, 1234, 80, 6, 0x01); // FIN
    tracker->Update(pFin, mark1, ConnectionDirection::kOutput);

    // Wait 4.1s. It should expire!
    waitTimer.expires_after(std::chrono::milliseconds(4100));
    co_await waitTimer.async_wait(Omni::Fiber::AsioUseFiber);
    EXPECT_FALSE(tracker->Lookup(pSyn, ConnectionDirection::kOutput).has_value());

    auto errStop = co_await tracker->Stop();
    EXPECT_FALSE(errStop);
    co_await tracker->WaitService();

    // Restore default timeouts
    ConnectionTracker::TcpEntry::SynTimeout = std::chrono::seconds(60);
    ConnectionTracker::TcpEntry::EstablishedTimeout = std::chrono::seconds(1200);
    ConnectionTracker::TcpEntry::FinTimeout = std::chrono::seconds(30);

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
      auto clientIp = boost::asio::ip::make_address_v4("10.0.0.1");
      auto serverIp = boost::asio::ip::make_address_v4("10.0.0.2");
      auto originalTcp = CreateIPv4Packet(clientIp, serverIp, 1234, 80, 6, 0x02); // TCP SYN from client

      // Update tracker with original TCP connection
      tracker->Update(originalTcp, mark1, ConnectionDirection::kOutput);

      // Now create an incoming ICMP Destination Unreachable packet from a router to the client
      auto routerIp = boost::asio::ip::make_address_v4("10.0.0.3");
      auto icmpPacket = CreateIcmp4DestUnreachable(routerIp, clientIp, originalTcp);

      // Lookup of the ICMP packet (direction: Input) should resolve the original TCP connection mark!
      auto res = tracker->Lookup(icmpPacket, ConnectionDirection::kInput);
      EXPECT_TRUE(res.has_value());
      EXPECT_EQ(&res->get(), &mark1);
    }

    // IPv6 association test
    {
      auto clientIp = boost::asio::ip::make_address_v6("fd00::1");
      auto serverIp = boost::asio::ip::make_address_v6("fd00::2");
      auto originalUdp = CreateIPv6Packet(clientIp, serverIp, 5678, 53, 17); // UDP from client

      tracker->Update(originalUdp, mark1, ConnectionDirection::kOutput);

      // Incoming ICMPv6 Destination Unreachable packet
      auto routerIp = boost::asio::ip::make_address_v6("fd00::3");
      auto icmpPacket = CreateIcmp6DestUnreachable(routerIp, clientIp, originalUdp);

      // Lookup of the ICMPv6 packet should resolve the original UDP connection mark!
      auto res = tracker->Lookup(icmpPacket, ConnectionDirection::kInput);
      EXPECT_TRUE(res.has_value());
      EXPECT_EQ(&res->get(), &mark1);
    }

    auto errStop = co_await tracker->Stop();
    EXPECT_FALSE(errStop);
    co_await tracker->WaitService();

    testPassed = true;
    co_return;
  });

  io.restart();
  io.run();
  EXPECT_TRUE(testPassed);
}
