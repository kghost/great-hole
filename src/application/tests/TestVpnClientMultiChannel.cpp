#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

#include <boost/asio.hpp>
#include <boost/lexical_cast.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "EndpointUdpDynMux.hpp"
#include "EventQueue.hpp"
#include "GetCurrentOmniFiber.hpp"
#include "OmniYield.hpp"
#include "Packet.hpp"
#include "ResolverStaticEndpoint.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"
#include "TimeTravel.hpp"
#include "Utils.hpp"
#include "Utils/Overload.hpp"
#include "VpnClientMultiChannel.hpp"

using namespace gh;

namespace {

struct TestTarget : public UdpDynMux::ChannelNotificationTarget {};

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

auto CreateTestIPv4Packet(const boost::asio::ip::address_v4& src, const boost::asio::ip::address_v4& dst,
                          uint8_t protocol, const std::vector<uint8_t>& payload) -> Packet {
  std::size_t totalLen = 20 + payload.size();
  Packet p(totalLen);
  auto data = p.Data();
  std::fill(data.begin(), data.end(), 0);
  data[0] = 0x45; // Version 4, IHL 5
  data[9] = protocol;

  // Total Length
  data[2] = (totalLen >> 8) & 0xFF;
  data[3] = totalLen & 0xFF;

  auto srcBytes = src.to_bytes();
  std::copy(srcBytes.begin(), srcBytes.end(), data.begin() + 12);
  auto destBytes = dst.to_bytes();
  std::copy(destBytes.begin(), destBytes.end(), data.begin() + 16);
  std::copy(payload.begin(), payload.end(), data.begin() + 20);
  return p;
}

auto CreateTestIPv6Packet(const boost::asio::ip::address_v6& src, const boost::asio::ip::address_v6& dst,
                          uint8_t nextHeader, const std::vector<uint8_t>& payload) -> Packet {
  std::size_t totalLen = 40 + payload.size();
  Packet p(totalLen);
  auto data = p.Data();
  std::fill(data.begin(), data.end(), 0);
  data[0] = 0x60; // Version 6
  data[6] = nextHeader;

  uint16_t payloadLen = payload.size();
  data[4] = (payloadLen >> 8) & 0xFF;
  data[5] = payloadLen & 0xFF;

  auto srcBytes = src.to_bytes();
  std::copy(srcBytes.begin(), srcBytes.end(), data.begin() + 8);
  auto destBytes = dst.to_bytes();
  std::copy(destBytes.begin(), destBytes.end(), data.begin() + 24);
  std::copy(payload.begin(), payload.end(), data.begin() + 40);
  return p;
}

auto CreateTcpPayload(uint16_t srcPort, uint16_t dstPort) -> std::vector<uint8_t> {
  std::vector<uint8_t> payload(20, 0);
  payload[0] = (srcPort >> 8) & 0xFF;
  payload[1] = srcPort & 0xFF;
  payload[2] = (dstPort >> 8) & 0xFF;
  payload[3] = dstPort & 0xFF;
  payload[12] = 0x50; // Data offset = 5 (20 bytes)
  return payload;
}

auto CreateUdpPayload(uint16_t srcPort, uint16_t dstPort) -> std::vector<uint8_t> {
  uint16_t totalLen = 8;
  std::vector<uint8_t> payload(totalLen, 0);
  payload[0] = (srcPort >> 8) & 0xFF;
  payload[1] = srcPort & 0xFF;
  payload[2] = (dstPort >> 8) & 0xFF;
  payload[3] = dstPort & 0xFF;
  payload[4] = (totalLen >> 8) & 0xFF;
  payload[5] = totalLen & 0xFF;
  return payload;
}

auto CreateIcmpEchoPayload(uint8_t type, uint16_t identifier) -> std::vector<uint8_t> {
  std::vector<uint8_t> payload(8, 0);
  payload[0] = type;
  payload[4] = (identifier >> 8) & 0xFF;
  payload[5] = identifier & 0xFF;
  return payload;
}

class MockEndpoint : public Endpoint {
public:
  MockEndpoint() = default;
  ~MockEndpoint() override = default;

  auto GetName() const -> std::string override { return "MockEndpoint"; }

  auto Read(Packet& p, Cancel& c) -> Omni::Fiber::Coroutine<ErrorCode> override {
    while (_ReadQueue.IsEmpty()) {
      if (c.IsTriggered()) {
        co_return Error(AppErrorCategory::kOperationAborted);
      }
      auto [cancelFired, queueFired] =
          co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(c.GetFiberCancelEvent(), [] -> void {}),
                                       Omni::Fiber::SelectPair(_ReadQueue, [] -> void {}));
      if (cancelFired) {
        co_return Error(AppErrorCategory::kOperationAborted);
      }
    }
    p = _ReadQueue.PopFront();
    co_return ErrorCode{};
  }

  auto Write(Packet& p, Cancel& c) -> Omni::Fiber::Coroutine<ErrorCode> override {
    if (c.IsTriggered()) {
      co_return Error(AppErrorCategory::kOperationAborted);
    }
    _WriteQueue.Push(std::move(p));
    co_return ErrorCode{};
  }

  void PushRead(Packet p) { _ReadQueue.Push(std::move(p)); }

  auto PopWrite(Cancel& c) -> Omni::Fiber::Coroutine<Packet> {
    while (_WriteQueue.IsEmpty()) {
      if (c.IsTriggered()) {
        co_return Packet();
      }
      auto [cancelFired, queueFired] =
          co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(c.GetFiberCancelEvent(), [] -> void {}),
                                       Omni::Fiber::SelectPair(_WriteQueue, [] -> void {}));
      if (cancelFired) {
        co_return Packet();
      }
    }
    co_return _WriteQueue.PopFront();
  }

  auto IsWriteEmpty() const -> bool { return _WriteQueue.IsEmpty(); }

protected:
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override { co_return ErrorCode{}; }
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override { co_return ErrorCode{}; }

private:
  Omni::Fiber::EventQueue<Packet> _ReadQueue;
  Omni::Fiber::EventQueue<Packet> _WriteQueue;
};
struct CallbackArgs {
  boost::asio::ip::address_v6 Src;
  boost::asio::ip::address_v6 Dst;
  uint16_t SrcPort = 0;
  uint16_t DstPort = 0;
  uint8_t Protocol = 0;
};

class CallbackSelector : public ConnectionTracker::Selector {
public:
  CallbackSelector(std::vector<CallbackArgs>& invocations) : _Invocations(invocations) {}

  auto SelectConnectionMark(const ConnectionTracker::ConnectionKey& key) -> std::shared_ptr<ConnectionMark> override {
    std::visit(
        Overload{[this](const ConnectionTracker::Ip4TcpKey& k) {
                   _Invocations.push_back(
                       CallbackArgs{MapToV6(k.LocalAddress), MapToV6(k.RemoteAddress), k.LocalPort, k.RemotePort, 6});
                 },
                 [this](const ConnectionTracker::Ip6TcpKey& k) {
                   _Invocations.push_back(CallbackArgs{k.LocalAddress, k.RemoteAddress, k.LocalPort, k.RemotePort, 6});
                 },
                 [this](const ConnectionTracker::Ip4UdpKey& k) {
                   _Invocations.push_back(
                       CallbackArgs{MapToV6(k.LocalAddress), MapToV6(k.RemoteAddress), k.LocalPort, k.RemotePort, 17});
                 },
                 [this](const ConnectionTracker::Ip6UdpKey& k) {
                   _Invocations.push_back(CallbackArgs{k.LocalAddress, k.RemoteAddress, k.LocalPort, k.RemotePort, 17});
                 },
                 [this](const ConnectionTracker::IcmpKey& k) {
                   _Invocations.push_back(
                       CallbackArgs{MapToV6(k.LocalAddress), MapToV6(k.RemoteAddress), k.Id, k.Id, 1});
                 },
                 [this](const ConnectionTracker::Icmp6Key& k) {
                   _Invocations.push_back(CallbackArgs{k.LocalAddress, k.RemoteAddress, k.Id, k.Id, 58});
                 }},
        key);
    return std::make_unique<VpnClientMultiChannel::Mark>(VpnClientMultiChannel::Mark::Discard{});
  }

private:
  std::vector<CallbackArgs>& _Invocations;
};

class RoutingSelector : public ConnectionTracker::Selector {
public:
  RoutingSelector(std::shared_ptr<VpnClientMultiChannelSession>& resolvedSession, int& selectorCalls)
      : _ResolvedSession(resolvedSession), _SelectorCalls(selectorCalls) {}

  auto SelectConnectionMark(const ConnectionTracker::ConnectionKey&) -> std::shared_ptr<ConnectionMark> override {
    return Handle();
  }

private:
  auto Handle() const -> std::shared_ptr<ConnectionMark> {
    _SelectorCalls++;
    if (_ResolvedSession) {
      return std::make_unique<VpnClientMultiChannel::Mark>(_ResolvedSession);
    }
    return std::make_unique<VpnClientMultiChannel::Mark>(VpnClientMultiChannel::Mark::Discard{});
  }

  std::shared_ptr<VpnClientMultiChannelSession>& _ResolvedSession;
  int& _SelectorCalls;
};

} // namespace

TEST(VpnClientMultiChannelTest, PacketParsingAndCallbackInvocation) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    std::vector<CallbackArgs> invocations;
    CallbackSelector selector(invocations);
    auto tracker = std::make_shared<ConnectionTracker>(io.get_executor());
    auto mockTun = std::make_shared<MockEndpoint>();
    auto udpServer = std::make_shared<UdpDynMux>(
        io.get_executor(), boost::asio::ip::udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
    auto connTrack = std::make_shared<VpnClientMultiChannel>(io.get_executor(), mockTun, udpServer, tracker, selector,
                                                             std::vector<std::shared_ptr<Filter>>{});
    EXPECT_FALSE(co_await connTrack->Start());

    Cancel cancelObj;

    // 1. TCP IPv4
    {
      auto srcIp = boost::asio::ip::make_address_v4("10.0.0.1");
      auto dstIp = boost::asio::ip::make_address_v4("192.168.1.1");
      auto p = CreateTestIPv4Packet(srcIp, dstIp, 6, CreateTcpPayload(1234, 80));
      mockTun->PushRead(std::move(p));
      co_await Omni::Fiber::OmniYield();

      EXPECT_EQ(invocations.size(), 1);
      if (invocations.size() < 1) {
        co_return;
      }
      EXPECT_EQ(invocations.back().Src, MapToV6(srcIp));
      EXPECT_EQ(invocations.back().Dst, MapToV6(dstIp));
      EXPECT_EQ(invocations.back().SrcPort, 1234);
      EXPECT_EQ(invocations.back().DstPort, 80);
      EXPECT_EQ(invocations.back().Protocol, 6);
    }

    // 2. UDP IPv6
    {
      auto srcIp = boost::asio::ip::make_address_v6("fd00::1");
      auto dstIp = boost::asio::ip::make_address_v6("fd00::2");
      auto p = CreateTestIPv6Packet(srcIp, dstIp, 17, CreateUdpPayload(5555, 6666));
      mockTun->PushRead(std::move(p));
      co_await Omni::Fiber::OmniYield();

      EXPECT_EQ(invocations.size(), 2);
      if (invocations.size() < 2) {
        co_return;
      }
      EXPECT_EQ(invocations.back().Src, srcIp);
      EXPECT_EQ(invocations.back().Dst, dstIp);
      EXPECT_EQ(invocations.back().SrcPort, 5555);
      EXPECT_EQ(invocations.back().DstPort, 6666);
      EXPECT_EQ(invocations.back().Protocol, 17);
    }

    // 3. ICMP Echo Request IPv4
    {
      auto srcIp = boost::asio::ip::make_address_v4("10.0.0.5");
      auto dstIp = boost::asio::ip::make_address_v4("10.0.0.6");
      auto p = CreateTestIPv4Packet(srcIp, dstIp, 1, CreateIcmpEchoPayload(8, 0xabcd));
      mockTun->PushRead(std::move(p));
      co_await Omni::Fiber::OmniYield();

      EXPECT_EQ(invocations.size(), 3);
      if (invocations.size() < 3) {
        co_return;
      }
      EXPECT_EQ(invocations.back().Src, MapToV6(srcIp));
      EXPECT_EQ(invocations.back().Dst, MapToV6(dstIp));
      EXPECT_EQ(invocations.back().SrcPort, 0xabcd);
      EXPECT_EQ(invocations.back().DstPort, 0xabcd);
      EXPECT_EQ(invocations.back().Protocol, 1);
    }

    // 4. ICMPv6 Echo Reply IPv6
    {
      auto srcIp = boost::asio::ip::make_address_v6("fe80::1");
      auto dstIp = boost::asio::ip::make_address_v6("fe80::2");
      auto p = CreateTestIPv6Packet(srcIp, dstIp, 58, CreateIcmpEchoPayload(129, 0x1234));
      mockTun->PushRead(std::move(p));
      co_await Omni::Fiber::OmniYield();

      EXPECT_EQ(invocations.size(), 4);
      if (invocations.size() < 4) {
        co_return;
      }
      EXPECT_EQ(invocations.back().Src, srcIp);
      EXPECT_EQ(invocations.back().Dst, dstIp);
      EXPECT_EQ(invocations.back().SrcPort, 0x1234);
      EXPECT_EQ(invocations.back().DstPort, 0x1234);
      EXPECT_EQ(invocations.back().Protocol, 58);
    }

    co_await connTrack->Stop();
    testPassed = true;
    co_return;
  });

  io.restart();
  io.run();
  EXPECT_TRUE(testPassed);
}

TEST(VpnClientMultiChannelTest, BidirectionalRoutingAndTimeoutPruning) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  Omni::TimeTravel::Client timeClient;
  AsioWarpListener listener(io);
  timeClient.RegisterListener(listener);

  auto udpClient = std::make_shared<UdpDynMux>(
      io.get_executor(), boost::asio::ip::udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  auto udpServer = std::make_shared<UdpDynMux>(
      io.get_executor(), boost::asio::ip::udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  UdpDynMux::PskType psk = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6};

  std::shared_ptr<VpnClientMultiChannelSession> resolvedSession;
  int selectorCalls = 0;

  RoutingSelector selector(resolvedSession, selectorCalls);
  auto tracker = std::make_shared<ConnectionTracker>(io.get_executor());
  auto mockTun = std::make_shared<MockEndpoint>();
  auto connTrack = std::make_shared<VpnClientMultiChannel>(io.get_executor(), mockTun, udpServer, tracker, selector,
                                                           std::vector<std::shared_ptr<Filter>>{});

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto& current = co_await Omni::Fiber::GetCurrentOmniFiber();

    // Start all services
    EXPECT_FALSE(co_await udpClient->Start());
    EXPECT_FALSE(co_await connTrack->Start());

    // Register channel on server side first so it can receive client initiates
    auto session = connTrack->RegisterChannel(psk, boost::lexical_cast<std::string>(udpClient->LocalEndpoint()));
    co_await connTrack->StartChannel(session);
    auto sharedSession = session.lock();
    EXPECT_NE(sharedSession, nullptr);
    if (sharedSession == nullptr) {
      co_return;
    }
    auto channel = sharedSession->StateMachine.GetData<VpnClientMultiChannelSession::State::kStarting>().Channel;
    EXPECT_NE(channel, nullptr);
    if (channel == nullptr) {
      co_return;
    }
    resolvedSession = sharedSession;

    // Connect client to server
    TestTarget clientTarget;
    auto resolver = std::make_shared<ResolverStaticEndpoint>(udpServer->LocalEndpoint());
    auto clientChannel = co_await udpClient->CreateChannel(psk, clientTarget, resolver);
    EXPECT_NE(clientChannel, nullptr);
    if (clientChannel == nullptr) {
      co_return;
    }

    // Wait for OnChannelEstablished to be invoked on connTrack
    // Server should accept client channel
    do {
      boost::asio::steady_timer waitTimer(io.get_executor());
      waitTimer.expires_after(std::chrono::milliseconds(10));
      co_await waitTimer.async_wait(Omni::Fiber::AsioUseFiber);
    } while (clientChannel->GetChannelState() != UdpDynMux::Channel::State::kRunning);

    Cancel cancelObj;

    // Test 1: Incoming packet from client channel
    // It should automatically create conntrack entry and be read from MockEndpoint (written by _TunPipeline)
    {
      auto srcIp = boost::asio::ip::make_address_v4("10.0.0.1");
      auto dstIp = boost::asio::ip::make_address_v4("10.0.0.2");
      auto p = CreateTestIPv4Packet(srcIp, dstIp, 17, CreateUdpPayload(5000, 6000));
      auto originalSize = p.DataSize();
      // Write from client to server (meaning it's an input packet to server conntrack)
      auto errWrite = co_await clientChannel->Write(p, cancelObj);
      EXPECT_FALSE(errWrite);

      Packet rxP = co_await mockTun->PopWrite(cancelObj);
      EXPECT_EQ(rxP.DataSize(), originalSize);
    }

    // Test 2: Outgoing packet from TunSide with reversed IP/ports
    // It should find the conntrack entry, write to resolvedChannel, and NOT call selector
    {
      auto srcIp = boost::asio::ip::make_address_v4("10.0.0.2"); // Reversed
      auto dstIp = boost::asio::ip::make_address_v4("10.0.0.1"); // Reversed
      auto p = CreateTestIPv4Packet(srcIp, dstIp, 17, CreateUdpPayload(6000, 5000));
      auto originalSize = p.DataSize();

      EXPECT_EQ(selectorCalls, 0);
      mockTun->PushRead(std::move(p));

      // Verify client received the packet
      Packet rxClient;
      auto errRead = co_await clientChannel->Read(rxClient, cancelObj);
      EXPECT_FALSE(errRead);
      EXPECT_EQ(rxClient.DataSize(), originalSize);
      EXPECT_EQ(selectorCalls, 0); // Still 0, conntrack mapped it automatically!
    }

    // Test 3: New outgoing packet (no conntrack entry exists)
    // It should call selector callback, create entry, and send
    {
      auto srcIp = boost::asio::ip::make_address_v4("10.0.0.3");
      auto dstIp = boost::asio::ip::make_address_v4("10.0.0.4");
      auto p = CreateTestIPv4Packet(srcIp, dstIp, 6, CreateTcpPayload(1111, 2222));
      auto originalSize = p.DataSize();

      EXPECT_EQ(selectorCalls, 0);
      mockTun->PushRead(std::move(p));

      // Verify client received the packet
      Packet rxClient;
      auto errRead = co_await clientChannel->Read(rxClient, cancelObj);
      EXPECT_FALSE(errRead);
      EXPECT_EQ(rxClient.DataSize(), originalSize);
      EXPECT_EQ(selectorCalls, 1); // Callback invoked!

      // Send again, should NOT call selector
      auto p2 = CreateTestIPv4Packet(srcIp, dstIp, 6, CreateTcpPayload(1111, 2222));
      mockTun->PushRead(std::move(p2));

      auto errRead2 = co_await clientChannel->Read(rxClient, cancelObj);
      EXPECT_FALSE(errRead2);
      EXPECT_EQ(selectorCalls, 1); // Still 1!
    }

    // Test 4: Conntrack pruning
    // Warp monotonic clock forward 61s so entry expires, then check if selector is called again
    {
      timeClient.FastForward(std::chrono::seconds(61));

      // Send packet for same connection
      auto srcIp = boost::asio::ip::make_address_v4("10.0.0.3");
      auto dstIp = boost::asio::ip::make_address_v4("10.0.0.4");
      auto p = CreateTestIPv4Packet(srcIp, dstIp, 6, CreateTcpPayload(1111, 2222));

      mockTun->PushRead(std::move(p));

      Packet rxClient;
      auto errRead = co_await clientChannel->Read(rxClient, cancelObj);
      EXPECT_FALSE(errRead);
      EXPECT_EQ(selectorCalls, 2); // Callback invoked again because entry timed out and was pruned!
    }

    // Cleanup
    co_await connTrack->Stop();
    co_await udpClient->Stop();

    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  io.restart();
  io.run();
  EXPECT_TRUE(testPassed);
}

TEST(VpnClientMultiChannelTest, SendPacketWithEstablishedConntrackToUnregisteredChannel) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto udpClient = std::make_shared<UdpDynMux>(
      io.get_executor(), boost::asio::ip::udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  auto udpServer = std::make_shared<UdpDynMux>(
      io.get_executor(), boost::asio::ip::udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  UdpDynMux::PskType psk = {1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 1, 2, 3, 4, 5, 6};

  std::shared_ptr<VpnClientMultiChannelSession> resolvedSession;
  int selectorCalls = 0;

  RoutingSelector selector(resolvedSession, selectorCalls);
  auto tracker = std::make_shared<ConnectionTracker>(io.get_executor());
  auto mockTun = std::make_shared<MockEndpoint>();
  auto connTrack = std::make_shared<VpnClientMultiChannel>(io.get_executor(), mockTun, udpServer, tracker, selector,
                                                           std::vector<std::shared_ptr<Filter>>{});

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto& current = co_await Omni::Fiber::GetCurrentOmniFiber();

    // Start all services
    EXPECT_FALSE(co_await udpClient->Start());
    EXPECT_FALSE(co_await connTrack->Start());

    // Register channel
    auto session = connTrack->RegisterChannel(psk, boost::lexical_cast<std::string>(udpClient->LocalEndpoint()));
    co_await connTrack->StartChannel(session);
    auto sharedSession = session.lock();
    EXPECT_NE(sharedSession, nullptr);
    if (sharedSession == nullptr) {
      co_return;
    }
    auto channel = sharedSession->StateMachine.GetData<VpnClientMultiChannelSession::State::kStarting>().Channel;
    EXPECT_NE(channel, nullptr);
    if (channel == nullptr) {
      co_return;
    }
    resolvedSession = sharedSession;

    // Connect client to server
    TestTarget clientTarget2;
    auto resolver2 = std::make_shared<ResolverStaticEndpoint>(udpServer->LocalEndpoint());
    auto clientChannel = co_await udpClient->CreateChannel(psk, clientTarget2, resolver2);
    EXPECT_NE(clientChannel, nullptr);

    // Wait for OnChannelEstablished
    do {
      boost::asio::steady_timer waitTimer(io.get_executor());
      waitTimer.expires_after(std::chrono::milliseconds(10));
      co_await waitTimer.async_wait(Omni::Fiber::AsioUseFiber);
    } while (clientChannel->GetChannelState() != UdpDynMux::Channel::State::kRunning);

    Cancel cancelObj;

    // 1. Send incoming packet to establish conntrack entry
    auto srcIp = boost::asio::ip::make_address_v4("10.0.0.1");
    auto dstIp = boost::asio::ip::make_address_v4("10.0.0.2");
    {
      auto p = CreateTestIPv4Packet(srcIp, dstIp, 17, CreateUdpPayload(5000, 6000));
      auto errWrite = co_await clientChannel->Write(p, cancelObj);
      EXPECT_FALSE(errWrite);

      Packet rxP = co_await mockTun->PopWrite(cancelObj);
    }

    // 2. Unregister the channel
    co_await connTrack->StopChannel(session);
    connTrack->UnregisterChannel(session);
    resolvedSession = nullptr;

    // 3. Send an outgoing packet matching the established connection key
    {
      auto p = CreateTestIPv4Packet(dstIp, srcIp, 17, CreateUdpPayload(6000, 5000));
      mockTun->PushRead(std::move(p));

      do {
        // Wait a brief moment to ensure no crash occurs and packet is dropped
        boost::asio::steady_timer waitTimer(io.get_executor());
        waitTimer.expires_after(std::chrono::milliseconds(10));
        co_await waitTimer.async_wait(Omni::Fiber::AsioUseFiber);
      } while (selectorCalls < 1);

      EXPECT_EQ(selectorCalls, 1);
    }

    // Cleanup
    co_await connTrack->Stop();
    co_await udpClient->Stop();

    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  io.restart();
  io.run();
  EXPECT_TRUE(testPassed);
}

TEST(VpnClientMultiChannelTest, MigrateTun) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    std::vector<CallbackArgs> invocations;
    CallbackSelector selector(invocations);
    auto tracker = std::make_shared<ConnectionTracker>(io.get_executor());
    auto mockTun1 = std::make_shared<MockEndpoint>();
    auto mockTun2 = std::make_shared<MockEndpoint>();
    auto udpServer = std::make_shared<UdpDynMux>(
        io.get_executor(), boost::asio::ip::udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
    auto connTrack = std::make_shared<VpnClientMultiChannel>(io.get_executor(), mockTun1, udpServer, tracker, selector,
                                                             std::vector<std::shared_ptr<Filter>>{});
    EXPECT_FALSE(co_await connTrack->Start());

    // 1. Send packet on mockTun1 and verify it gets processed.
    {
      auto srcIp = boost::asio::ip::make_address_v4("10.0.0.1");
      auto dstIp = boost::asio::ip::make_address_v4("192.168.1.1");
      auto p = CreateTestIPv4Packet(srcIp, dstIp, 6, CreateTcpPayload(1234, 80));
      mockTun1->PushRead(std::move(p));
      co_await Omni::Fiber::OmniYield();

      EXPECT_EQ(invocations.size(), 1);
      if (invocations.size() < 1) {
        co_return;
      }
      EXPECT_EQ(invocations.back().Src, MapToV6(srcIp));
      EXPECT_EQ(invocations.back().Dst, MapToV6(dstIp));
    }

    // 2. Migrate to mockTun2.
    auto err = co_await connTrack->MigrateTun(mockTun2);
    EXPECT_FALSE(err);

    // 3. Send packet on mockTun1 and verify it does NOT get processed.
    {
      auto srcIp = boost::asio::ip::make_address_v4("10.0.0.1");
      auto dstIp = boost::asio::ip::make_address_v4("192.168.1.1");
      auto p = CreateTestIPv4Packet(srcIp, dstIp, 6, CreateTcpPayload(1234, 80));
      mockTun1->PushRead(std::move(p));
      co_await Omni::Fiber::OmniYield();

      // Invocations count should still be 1 because mockTun1 is stopped/disconnected.
      EXPECT_EQ(invocations.size(), 1);
    }

    // 4. Send packet on mockTun2 and verify it gets processed.
    {
      auto srcIp = boost::asio::ip::make_address_v4("10.0.0.2");
      auto dstIp = boost::asio::ip::make_address_v4("192.168.1.2");
      auto p = CreateTestIPv4Packet(srcIp, dstIp, 6, CreateTcpPayload(1234, 80));
      mockTun2->PushRead(std::move(p));
      co_await Omni::Fiber::OmniYield();

      EXPECT_EQ(invocations.size(), 2);
      if (invocations.size() < 2) {
        co_return;
      }
      EXPECT_EQ(invocations.back().Src, MapToV6(srcIp));
      EXPECT_EQ(invocations.back().Dst, MapToV6(dstIp));
    }

    co_await connTrack->Stop();
    testPassed = true;
    co_return;
  });

  io.restart();
  io.run();
  EXPECT_TRUE(testPassed);
}

TEST(VpnClientMultiChannelTest, TrafficStatsWithRtt) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto udpClient = std::make_shared<UdpDynMux>(
      io.get_executor(), boost::asio::ip::udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  auto udpServer = std::make_shared<UdpDynMux>(
      io.get_executor(), boost::asio::ip::udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  UdpDynMux::PskType psk = {1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 1, 2, 3, 4, 5, 6};

  std::shared_ptr<VpnClientMultiChannelSession> resolvedSession;
  int selectorCalls = 0;

  RoutingSelector selector(resolvedSession, selectorCalls);
  auto tracker = std::make_shared<ConnectionTracker>(io.get_executor());
  auto mockTun = std::make_shared<MockEndpoint>();
  auto connTrack = std::make_shared<VpnClientMultiChannel>(io.get_executor(), mockTun, udpServer, tracker, selector,
                                                           std::vector<std::shared_ptr<Filter>>{});

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto& current = co_await Omni::Fiber::GetCurrentOmniFiber();

    // Start all services
    EXPECT_FALSE(co_await udpClient->Start());
    EXPECT_FALSE(co_await connTrack->Start());

    // Before session channel is established/registered, stats should be nullopt
    auto session = connTrack->RegisterChannel(psk, boost::lexical_cast<std::string>(udpClient->LocalEndpoint()));
    co_await connTrack->StartChannel(session);
    auto sharedSession = session.lock();
    EXPECT_NE(sharedSession, nullptr);
    if (sharedSession == nullptr) {
      co_return;
    }
    auto channel = sharedSession->StateMachine.GetData<VpnClientMultiChannelSession::State::kStarting>().Channel;
    EXPECT_NE(channel, nullptr);
    if (channel == nullptr) {
      co_return;
    }
    resolvedSession = sharedSession;

    auto initialStats = connTrack->GetStats(session);
    EXPECT_FALSE(initialStats.has_value());

    // Connect client to server to establish pipeline and running session
    TestTarget clientTarget3;
    auto resolver3 = std::make_shared<ResolverStaticEndpoint>(udpServer->LocalEndpoint());
    auto clientChannel = co_await udpClient->CreateChannel(psk, clientTarget3, resolver3);
    EXPECT_NE(clientChannel, nullptr);

    // Wait for OnChannelEstablished
    do {
      boost::asio::steady_timer waitTimer(io.get_executor());
      waitTimer.expires_after(std::chrono::milliseconds(10));
      co_await waitTimer.async_wait(Omni::Fiber::AsioUseFiber);
    } while (clientChannel->GetChannelState() != UdpDynMux::Channel::State::kRunning);

    // Now stats should be available and RttMs should be initialized/retrieved
    auto stats = connTrack->GetStats(session);
    EXPECT_TRUE(stats.has_value());
    if (stats.has_value()) {
      EXPECT_EQ(stats->RttMs, 0); // initial default value
    }

    // Cleanup
    co_await connTrack->Stop();
    co_await udpClient->Stop();

    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  io.restart();
  io.run();
  EXPECT_TRUE(testPassed);
}

class MockSessionStateListener : public VpnClientMultiChannel::SessionStateListener {
public:
  std::vector<std::string> Events;

  void OnSessionStarting(const std::weak_ptr<VpnClientMultiChannelSession>& /*session*/) override {
    Events.push_back("Starting");
  }
  void OnSessionRunning(const std::weak_ptr<VpnClientMultiChannelSession>& /*session*/) override {
    Events.push_back("Running");
  }
  void OnSessionStopping(const std::weak_ptr<VpnClientMultiChannelSession>& /*session*/) override {
    Events.push_back("Stopping");
  }
  void OnSessionStopped(const std::weak_ptr<VpnClientMultiChannelSession>& /*session*/) override {
    Events.push_back("Stopped");
  }
  void OnSessionFailed(const std::weak_ptr<VpnClientMultiChannelSession>& /*session*/,
                       const std::string& error) override {
    Events.push_back("Failed: " + error);
  }
};

TEST(VpnClientMultiChannelTest, SessionStateTransitions) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto udpClient = std::make_shared<UdpDynMux>(
      io.get_executor(), boost::asio::ip::udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  auto udpServer = std::make_shared<UdpDynMux>(
      io.get_executor(), boost::asio::ip::udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  UdpDynMux::PskType psk = {1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4};

  std::shared_ptr<VpnClientMultiChannelSession> resolvedSession;
  int selectorCalls = 0;

  RoutingSelector selector(resolvedSession, selectorCalls);
  auto tracker = std::make_shared<ConnectionTracker>(io.get_executor());
  auto mockTun = std::make_shared<MockEndpoint>();
  MockSessionStateListener stateListener;

  auto connTrack = std::make_shared<VpnClientMultiChannel>(io.get_executor(), mockTun, udpServer, tracker, selector,
                                                           std::vector<std::shared_ptr<Filter>>{}, stateListener);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto& current = co_await Omni::Fiber::GetCurrentOmniFiber();

    EXPECT_FALSE(co_await udpClient->Start());
    EXPECT_FALSE(co_await connTrack->Start());

    // Register channel - initial state is kNone
    auto sessionWeak = connTrack->RegisterChannel(psk, boost::lexical_cast<std::string>(udpClient->LocalEndpoint()));
    auto session = sessionWeak.lock();
    EXPECT_NE(session, nullptr);
    EXPECT_TRUE(session->StateMachine.IsState<VpnClientMultiChannelSession::State::kNone>());
    EXPECT_TRUE(stateListener.Events.empty());

    // Start channel - state transitions to kStarting and fires OnSessionStarting
    co_await connTrack->StartChannel(sessionWeak);
    EXPECT_TRUE(session->StateMachine.IsState<VpnClientMultiChannelSession::State::kStarting>());
    EXPECT_EQ(stateListener.Events.size(), 1);
    EXPECT_EQ(stateListener.Events[0], "Starting");

    // Establish channel connection from client
    TestTarget clientTarget;
    auto resolver = std::make_shared<ResolverStaticEndpoint>(udpServer->LocalEndpoint());
    auto clientChannel = co_await udpClient->CreateChannel(psk, clientTarget, resolver);
    EXPECT_NE(clientChannel, nullptr);

    do {
      boost::asio::steady_timer waitTimer(io.get_executor());
      waitTimer.expires_after(std::chrono::milliseconds(10));
      co_await waitTimer.async_wait(Omni::Fiber::AsioUseFiber);
    } while (clientChannel->GetChannelState() != UdpDynMux::Channel::State::kRunning);

    // After establishment, state transitions to kRunning and fires OnSessionRunning
    EXPECT_TRUE(session->StateMachine.IsState<VpnClientMultiChannelSession::State::kRunning>());
    EXPECT_EQ(stateListener.Events.size(), 2);
    EXPECT_EQ(stateListener.Events[1], "Running");

    // Stop channel - state transitions kRunning -> kStopping -> kNone and fires OnSessionStopping & OnSessionStopped
    co_await connTrack->StopChannel(sessionWeak);
    EXPECT_TRUE(session->StateMachine.IsState<VpnClientMultiChannelSession::State::kNone>());
    EXPECT_EQ(stateListener.Events.size(), 4);
    EXPECT_EQ(stateListener.Events[2], "Stopping");
    EXPECT_EQ(stateListener.Events[3], "Stopped");

    connTrack->UnregisterChannel(sessionWeak);

    co_await connTrack->Stop();
    co_await udpClient->Stop();
    co_await current.WaitAll();

    testPassed = true;
    co_return;
  });

  io.restart();
  io.run();
  EXPECT_TRUE(testPassed);
}

TEST(VpnClientMultiChannelTest, SessionReconnectOnUnexpectedChannelClose) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  Omni::TimeTravel::Client timeClient;
  AsioWarpListener listener(io);
  timeClient.RegisterListener(listener);

  auto udpClient = std::make_shared<UdpDynMux>(
      io.get_executor(), boost::asio::ip::udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  auto udpServer = std::make_shared<UdpDynMux>(
      io.get_executor(), boost::asio::ip::udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  UdpDynMux::PskType psk = {5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 8};

  std::shared_ptr<VpnClientMultiChannelSession> resolvedSession;
  int selectorCalls = 0;

  RoutingSelector selector(resolvedSession, selectorCalls);
  auto tracker = std::make_shared<ConnectionTracker>(io.get_executor());
  auto mockTun = std::make_shared<MockEndpoint>();
  MockSessionStateListener stateListener;

  auto connTrack = std::make_shared<VpnClientMultiChannel>(io.get_executor(), mockTun, udpServer, tracker, selector,
                                                           std::vector<std::shared_ptr<Filter>>{}, stateListener);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto& current = co_await Omni::Fiber::GetCurrentOmniFiber();

    EXPECT_FALSE(co_await udpClient->Start());
    EXPECT_FALSE(co_await connTrack->Start());

    auto sessionWeak = connTrack->RegisterChannel(psk, boost::lexical_cast<std::string>(udpClient->LocalEndpoint()));
    auto session = sessionWeak.lock();

    co_await connTrack->StartChannel(sessionWeak);

    TestTarget clientTarget;
    auto resolver = std::make_shared<ResolverStaticEndpoint>(udpServer->LocalEndpoint());
    auto clientChannel = co_await udpClient->CreateChannel(psk, clientTarget, resolver);

    do {
      boost::asio::steady_timer waitTimer(io.get_executor());
      waitTimer.expires_after(std::chrono::milliseconds(10));
      co_await waitTimer.async_wait(Omni::Fiber::AsioUseFiber);
    } while (clientChannel->GetChannelState() != UdpDynMux::Channel::State::kRunning);

    EXPECT_TRUE(session->StateMachine.IsState<VpnClientMultiChannelSession::State::kRunning>());
    EXPECT_EQ(stateListener.Events.size(), 2);
    EXPECT_EQ(stateListener.Events[0], "Starting");
    EXPECT_EQ(stateListener.Events[1], "Running");

    // Close client channel unexpectedly (simulating network disconnect)
    co_await udpClient->RemoveChannel(clientChannel);

    while (!session->StateMachine.IsState<VpnClientMultiChannelSession::State::kStarting>()) {
      timeClient.FastForward(std::chrono::seconds(61));
      boost::asio::steady_timer waitTimer(io.get_executor());
      waitTimer.expires_after(std::chrono::milliseconds(10));
      co_await waitTimer.async_wait(Omni::Fiber::AsioUseFiber);
    };

    // Verify session state moved back to kStarting and OnSessionStarting fired again (ready for reconnect)
    EXPECT_TRUE(session->StateMachine.IsState<VpnClientMultiChannelSession::State::kStarting>());
    EXPECT_EQ(stateListener.Events.size(), 3);
    EXPECT_EQ(stateListener.Events[2], "Starting");

    // Clean up
    co_await connTrack->StopChannel(sessionWeak);
    connTrack->UnregisterChannel(sessionWeak);

    co_await connTrack->Stop();
    co_await udpClient->Stop();
    co_await current.WaitAll();

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
