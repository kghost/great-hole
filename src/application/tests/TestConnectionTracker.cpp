#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "ConnectionTracker.hpp"
#include "Coroutine.hpp"
#include "Packet.hpp"
#include "PacketHeader.hpp"
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
  auto GetDescription() const -> std::string override { return _Name; }
  auto Validate() const -> bool override { return Valid; }

  bool Valid = true;

private:
  std::string _Name;
};

class TestDiscardMark : public ConnectionMark {
public:
  auto GetDescription() const -> std::string override { return "Discard"; }
};

class TestBypassMark : public ConnectionMark {
public:
  auto GetDescription() const -> std::string override { return "Bypass"; }
};

inline TestDiscardMark g_TestDiscardMark;

class ReferenceMark : public ConnectionMark {
public:
  explicit ReferenceMark(ConnectionMark& mark) : _Mark(mark) {}
  auto GetDescription() const -> std::string override { return _Mark.GetDescription(); }
  auto Validate() const -> bool override { return _Mark.Validate(); }
  auto GetReferencedMark() const -> ConnectionMark& { return _Mark; }

private:
  ConnectionMark& _Mark;
};

class MockSelector : public ConnectionTracker::Selector {
public:
  explicit MockSelector(ConnectionMark& result) : Result(&result) {}

  auto Select(const ConnectionTracker::ConnectionKey&) -> Action override {
    return ConnectionTracker::Selector::Action(CloneResult());
  }

  mutable ConnectionMark* Result = nullptr;

private:
  auto CloneResult() const -> std::shared_ptr<ConnectionMark> {
    if (Result) {
      return std::make_unique<ReferenceMark>(*Result);
    }
    return std::make_unique<ReferenceMark>(g_TestDiscardMark);
  }
};

class ConstantSelector : public ConnectionTracker::Selector {
public:
  explicit ConstantSelector(ConnectionMark& mark) : _Mark(mark) {}
  auto Select(const ConnectionTracker::ConnectionKey&) -> Action override {
    return ConnectionTracker::Selector::Action(std::make_unique<ReferenceMark>(_Mark));
  }

private:
  ConnectionMark& _Mark;
};

static auto GetMark(const std::expected<std::shared_ptr<ConnectionMark>, ErrorCode>& res) -> ConnectionMark* {
  if (res.has_value()) {
    ConnectionMark* mark = res.value().get();
    if (auto* refMark = dynamic_cast<ReferenceMark*>(mark)) {
      return &refMark->GetReferencedMark();
    }
    return mark;
  }
  return nullptr;
}

auto CreatePacket(const std::vector<uint8_t>& bytes) -> Packet {
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

    auto connections = tracker->GetConnections();
    EXPECT_EQ(connections.size(), 2);
    if (connections.size() == 2) {
      EXPECT_FALSE(connections[0].Mark.empty());
      EXPECT_FALSE(connections[1].Mark.empty());
    }

    selector.Result = &g_TestDiscardMark;
    tracker->Clear();
    EXPECT_TRUE(tracker->GetConnections().empty());
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

class SnatSelector : public ConnectionTracker::Selector {
public:
  SnatSelector(ConnectionMark& mark, ConnectionTracker::Selector::Action::Snat4 snat)
      : _Mark(mark), _Snat(std::move(snat)) {}

  auto Select(const ConnectionTracker::ConnectionKey&) -> Action override {
    return Action(std::make_unique<ReferenceMark>(_Mark), _Snat);
  }

private:
  ConnectionMark& _Mark;
  ConnectionTracker::Selector::Action::Snat4 _Snat;
};

static auto ChecksumOfWords(std::span<const uint16_t> words, std::span<const uint8_t> oddTail = {}) -> uint16_t {
  uint32_t sum = 0;
  for (uint16_t w : words) {
    sum += w;
  }
  if (!oddTail.empty()) {
    sum += static_cast<uint16_t>(oddTail[0]);
  }
  while (sum >> 16) {
    sum = (sum & 0xFFFF) + (sum >> 16);
  }
  return static_cast<uint16_t>(sum);
}

static auto ValidateIp4HeaderChecksum(const IPv4Header* ip4) -> bool {
  auto span = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(ip4), ip4->GetIhl() * 4);
  auto wordSpan = std::span<const uint16_t>(reinterpret_cast<const uint16_t*>(span.data()), span.size() / 2);
  return ChecksumOfWords(wordSpan) == 0xFFFF;
}

static auto ValidateTcp4Checksum(const IPv4Header* ip4, const TCPHeader* tcp, std::span<const uint8_t> payload) -> bool {
  std::vector<uint16_t> words;
  const auto* srcPtr = reinterpret_cast<const uint16_t*>(&ip4->SrcIp);
  words.push_back(srcPtr[0]); words.push_back(srcPtr[1]);
  const auto* dstPtr = reinterpret_cast<const uint16_t*>(&ip4->DestIp);
  words.push_back(dstPtr[0]); words.push_back(dstPtr[1]);
  words.push_back(ArchEndian(static_cast<uint16_t>(IPProtocol::TCP)));
  uint16_t tcpLen = static_cast<uint16_t>(ip4->GetTotalLength() - ip4->GetIhl() * 4);
  words.push_back(ArchEndian(tcpLen));

  auto tcpSpan = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(tcp), tcp->GetDataOffset() * 4);
  auto tcpWordSpan = std::span<const uint16_t>(reinterpret_cast<const uint16_t*>(tcpSpan.data()), tcpSpan.size() / 2);
  for (uint16_t w : tcpWordSpan) {
    words.push_back(w);
  }

  auto payloadWordSpan = std::span<const uint16_t>(reinterpret_cast<const uint16_t*>(payload.data()), payload.size() / 2);
  auto oddTail = payload.subspan(payload.size() / 2 * 2);
  for (uint16_t w : payloadWordSpan) {
    words.push_back(w);
  }
  return ChecksumOfWords(words, oddTail) == 0xFFFF;
}

static auto ValidateTcp6Checksum(const IPv6Header* ip6, const TCPHeader* tcp, std::span<const uint8_t> payload) -> bool {
  std::vector<uint16_t> words;
  const auto* srcPtr = reinterpret_cast<const uint16_t*>(ip6->SrcIp.data());
  for (int i = 0; i < 8; ++i) words.push_back(srcPtr[i]);
  const auto* dstPtr = reinterpret_cast<const uint16_t*>(ip6->DestIp.data());
  for (int i = 0; i < 8; ++i) words.push_back(dstPtr[i]);
  uint32_t len = ArchEndian(ip6->GetPayloadLength());
  const auto* lenPtr = reinterpret_cast<const uint16_t*>(&len);
  words.push_back(lenPtr[0]); words.push_back(lenPtr[1]);
  uint32_t nxt = ArchEndian(static_cast<uint32_t>(IPProtocol::TCP));
  const auto* nxtPtr = reinterpret_cast<const uint16_t*>(&nxt);
  words.push_back(nxtPtr[0]); words.push_back(nxtPtr[1]);

  auto tcpSpan = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(tcp), tcp->GetDataOffset() * 4);
  auto tcpWordSpan = std::span<const uint16_t>(reinterpret_cast<const uint16_t*>(tcpSpan.data()), tcpSpan.size() / 2);
  for (uint16_t w : tcpWordSpan) {
    words.push_back(w);
  }

  auto payloadWordSpan = std::span<const uint16_t>(reinterpret_cast<const uint16_t*>(payload.data()), payload.size() / 2);
  auto oddTail = payload.subspan(payload.size() / 2 * 2);
  for (uint16_t w : payloadWordSpan) {
    words.push_back(w);
  }
  return ChecksumOfWords(words, oddTail) == 0xFFFF;
}

TEST(ConnectionTrackerTest, SnatOutputAndInputIPv4) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    MockConnectionMark mark1("mark1");
    SnatSelector snatSelector(mark1, ConnectionTracker::Selector::Action::Snat4{
                                         .LocalAddress = boost::asio::ip::make_address_v4("10.0.0.1"), .LocalPort = 54321});
    ConstantSelector dummySelector(mark1);

    auto tracker = std::make_shared<ConnectionTracker>(io.get_executor());
    auto errStart = co_await tracker->Start();
    EXPECT_FALSE(errStart);

    auto pOut = CreatePacket(test::captured::Ip4TcpSyn);
    const auto* ip4Orig = reinterpret_cast<const IPv4Header*>(pOut.Data().data());
    auto l4Orig = pOut.Data().subspan(ip4Orig->GetIhl() * 4);
    auto* tcpOrig = reinterpret_cast<TCPHeader*>(l4Orig.data());

    // Initialize valid initial TCP checksum on test packet before SNAT
    tcpOrig->Checksum = 0;
    tcpOrig->Checksum = ~ChecksumOfWords(
        [&] {
          std::vector<uint16_t> words;
          const auto* srcPtr = reinterpret_cast<const uint16_t*>(&ip4Orig->SrcIp);
          words.push_back(srcPtr[0]); words.push_back(srcPtr[1]);
          const auto* dstPtr = reinterpret_cast<const uint16_t*>(&ip4Orig->DestIp);
          words.push_back(dstPtr[0]); words.push_back(dstPtr[1]);
          words.push_back(ArchEndian(static_cast<uint16_t>(IPProtocol::TCP)));
          uint16_t tcpLen = static_cast<uint16_t>(ip4Orig->GetTotalLength() - ip4Orig->GetIhl() * 4);
          words.push_back(ArchEndian(tcpLen));
          auto tcpSpan = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(tcpOrig), tcpOrig->GetDataOffset() * 4);
          auto tcpWordSpan = std::span<const uint16_t>(reinterpret_cast<const uint16_t*>(tcpSpan.data()), tcpSpan.size() / 2);
          for (uint16_t w : tcpWordSpan) words.push_back(w);
          return words;
        }());

    // Outbound packet: SNAT should rewrite SrcIp to 10.0.0.1 and SrcPort to 54321
    auto resOut = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(pOut, snatSelector);
    EXPECT_TRUE(resOut.has_value());

    auto outSpan = pOut.Data();
    const auto* ip4Out = reinterpret_cast<const IPv4Header*>(outSpan.data());
    EXPECT_EQ(boost::asio::ip::make_address_v4(ip4Out->GetSrcIp()), boost::asio::ip::make_address_v4("10.0.0.1"));
    auto l4Out = outSpan.subspan(ip4Out->GetIhl() * 4);
    const auto* tcpOut = reinterpret_cast<const TCPHeader*>(l4Out.data());
    EXPECT_EQ(tcpOut->GetSrcPort(), 54321);

    EXPECT_TRUE(ValidateIp4HeaderChecksum(ip4Out));
    EXPECT_TRUE(ValidateTcp4Checksum(ip4Out, tcpOut, l4Out.subspan(tcpOut->GetDataOffset() * 4)));

    // Inbound reply packet: DestIp = 10.0.0.1, DestPort = 54321
    // Build reply packet matching NAT key
    auto pIn = CreatePacket(test::captured::Ip4TcpSyn);
    auto inSpan = pIn.Data();
    auto* ip4In = reinterpret_cast<IPv4Header*>(inSpan.data());
    // Swap original src/dst to form incoming reply, set DestIp=10.0.0.1, DestPort=54321
    uint32_t origSrc = ip4In->SrcIp;
    uint32_t origDst = ip4In->DestIp;
    ip4In->SrcIp = origDst;
    ip4In->DestIp = ArchEndian(boost::asio::ip::make_address_v4("10.0.0.1").to_uint());
    auto l4In = inSpan.subspan(ip4In->GetIhl() * 4);
    auto* tcpIn = reinterpret_cast<TCPHeader*>(l4In.data());
    uint16_t origSrcPortHost = tcpIn->GetSrcPort();
    uint16_t origDstPortNet = tcpIn->DestPort;
    tcpIn->SrcPort = origDstPortNet;
    tcpIn->DestPort = ArchEndian(static_cast<uint16_t>(54321));

    // Recalculate initial checksums for incoming reply packet before NAT
    ip4In->Checksum = 0;
    tcpIn->Checksum = 0;
    uint16_t ip4InSum = ~ChecksumOfWords(std::span<const uint16_t>(reinterpret_cast<const uint16_t*>(ip4In), ip4In->GetIhl() * 2));
    ip4In->Checksum = ip4InSum;
    tcpIn->Checksum = ~ChecksumOfWords(
        [&] {
          std::vector<uint16_t> words;
          const auto* srcPtr = reinterpret_cast<const uint16_t*>(&ip4In->SrcIp);
          words.push_back(srcPtr[0]); words.push_back(srcPtr[1]);
          const auto* dstPtr = reinterpret_cast<const uint16_t*>(&ip4In->DestIp);
          words.push_back(dstPtr[0]); words.push_back(dstPtr[1]);
          words.push_back(ArchEndian(static_cast<uint16_t>(IPProtocol::TCP)));
          uint16_t tcpLen = static_cast<uint16_t>(ip4In->GetTotalLength() - ip4In->GetIhl() * 4);
          words.push_back(ArchEndian(tcpLen));
          auto tcpSpan = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(tcpIn), tcpIn->GetDataOffset() * 4);
          auto tcpWordSpan = std::span<const uint16_t>(reinterpret_cast<const uint16_t*>(tcpSpan.data()), tcpSpan.size() / 2);
          for (uint16_t w : tcpWordSpan) words.push_back(w);
          return words;
        }());

    // Input lookup should de-NAT DestIp back to original local address and DestPort back to original local port
    auto resIn = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionInput>(pIn, dummySelector);
    EXPECT_TRUE(resIn.has_value());

    EXPECT_EQ(ip4In->GetDestIp(), ArchEndian(origSrc));
    EXPECT_EQ(tcpIn->GetDestPort(), origSrcPortHost);

    EXPECT_TRUE(ValidateIp4HeaderChecksum(ip4In));
    EXPECT_TRUE(ValidateTcp4Checksum(ip4In, tcpIn, l4In.subspan(tcpIn->GetDataOffset() * 4)));

    auto errStop = co_await tracker->Stop();
    EXPECT_FALSE(errStop);

    testPassed = true;
    co_return;
  });

  io.restart();
  io.run();
  EXPECT_TRUE(testPassed);
}

class Snat6Selector : public ConnectionTracker::Selector {
public:
  Snat6Selector(ConnectionMark& mark, ConnectionTracker::Selector::Action::Snat6 snat)
      : _Mark(mark), _Snat(std::move(snat)) {}

  auto Select(const ConnectionTracker::ConnectionKey&) -> Action override {
    return Action(std::make_unique<ReferenceMark>(_Mark), _Snat);
  }

private:
  ConnectionMark& _Mark;
  ConnectionTracker::Selector::Action::Snat6 _Snat;
};

TEST(ConnectionTrackerTest, SnatOutputAndInputIPv6) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    MockConnectionMark mark1("mark1");
    Snat6Selector snatSelector(mark1, ConnectionTracker::Selector::Action::Snat6{
                                          .LocalAddress = boost::asio::ip::make_address_v6("2001:db8::1"), .LocalPort = 54321});
    ConstantSelector dummySelector(mark1);

    auto tracker = std::make_shared<ConnectionTracker>(io.get_executor());
    auto errStart = co_await tracker->Start();
    EXPECT_FALSE(errStart);

    auto pOut = CreatePacket(test::captured::Ip6TcpSyn);
    const auto* ip6Orig = reinterpret_cast<const IPv6Header*>(pOut.Data().data());
    auto l4Orig = pOut.Data().subspan(sizeof(IPv6Header));
    auto* tcpOrig = reinterpret_cast<TCPHeader*>(l4Orig.data());

    // Initialize valid initial TCP checksum on test packet before SNAT
    tcpOrig->Checksum = 0;
    tcpOrig->Checksum = ~ChecksumOfWords(
        [&] {
          std::vector<uint16_t> words;
          const auto* srcPtr = reinterpret_cast<const uint16_t*>(ip6Orig->SrcIp.data());
          for (int i = 0; i < 8; ++i) words.push_back(srcPtr[i]);
          const auto* dstPtr = reinterpret_cast<const uint16_t*>(ip6Orig->DestIp.data());
          for (int i = 0; i < 8; ++i) words.push_back(dstPtr[i]);
          uint32_t len = ArchEndian(ip6Orig->GetPayloadLength());
          const auto* lenPtr = reinterpret_cast<const uint16_t*>(&len);
          words.push_back(lenPtr[0]); words.push_back(lenPtr[1]);
          uint32_t nxt = ArchEndian(static_cast<uint32_t>(IPProtocol::TCP));
          const auto* nxtPtr = reinterpret_cast<const uint16_t*>(&nxt);
          words.push_back(nxtPtr[0]); words.push_back(nxtPtr[1]);
          auto tcpSpan = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(tcpOrig), tcpOrig->GetDataOffset() * 4);
          auto tcpWordSpan = std::span<const uint16_t>(reinterpret_cast<const uint16_t*>(tcpSpan.data()), tcpSpan.size() / 2);
          for (uint16_t w : tcpWordSpan) words.push_back(w);
          return words;
        }());

    // Outbound packet: SNAT should rewrite SrcIp to 2001:db8::1 and SrcPort to 54321
    auto resOut = tracker->LookupAndUpdate<ConnectionTracker::ConnectionDirectionOutput>(pOut, snatSelector);
    EXPECT_TRUE(resOut.has_value());

    auto outSpan = pOut.Data();
    const auto* ip6Out = reinterpret_cast<const IPv6Header*>(outSpan.data());
    EXPECT_EQ(boost::asio::ip::make_address_v6(ip6Out->SrcIp), boost::asio::ip::make_address_v6("2001:db8::1"));
    auto l4Out = outSpan.subspan(sizeof(IPv6Header));
    const auto* tcpOut = reinterpret_cast<const TCPHeader*>(l4Out.data());
    EXPECT_EQ(tcpOut->GetSrcPort(), 54321);

    EXPECT_TRUE(ValidateTcp6Checksum(ip6Out, tcpOut, l4Out.subspan(tcpOut->GetDataOffset() * 4)));

    auto errStop = co_await tracker->Stop();
    EXPECT_FALSE(errStop);

    testPassed = true;
    co_return;
  });

  io.restart();
  io.run();
  EXPECT_TRUE(testPassed);
}

auto main(int argc, char* argv[]) -> int {
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
