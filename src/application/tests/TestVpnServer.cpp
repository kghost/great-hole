#include <memory>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "EndpointTunSplitIp.hpp"
#include "EndpointUdpDynMux.hpp"
#include "Filter.hpp"
#include "GetCurrentFiber.hpp"
#include "Pipeline.hpp"
#include "ResolverStaticEndpoint.hpp"
#include "Utils.hpp"
#include "VpnServer.hpp"

using namespace gh;

namespace {

class MockFilter : public Filter {
public:
  MockFilter(int& counter) : _Counter(counter) {}
  ~MockFilter() override = default;

  Omni::Fiber::Coroutine<boost::system::error_code> Pipe(Packet& /*p*/, Cancel& /*c*/) override {
    _Counter++;
    co_return boost::system::error_code{};
  }

private:
  int& _Counter;
};

} // namespace

TEST(VpnServerTest, ConstructorStoresFiltersAndAppliesThem) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  int fds[2];
  ASSERT_GE(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds), 0);
  int testFd = fds[0];
  int externalFd = fds[1];

  auto tunSplit = std::make_shared<EndpointTunSplitIp>(io.get_executor(), "test", testFd);
  auto udpDynMux = std::make_shared<UdpDynMux>(io.get_executor());
  int filterCounter = 0;
  auto mockFilter = std::make_shared<MockFilter>(filterCounter);
  std::vector<std::shared_ptr<Filter>> filters = {mockFilter};

  auto vpnServer = std::make_shared<VpnServer>(tunSplit, udpDynMux, filters);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto tunErr = co_await tunSplit->Start();
    EXPECT_FALSE(tunErr);

    auto udpMuxErr = co_await udpDynMux->Start();
    EXPECT_FALSE(udpMuxErr);

    auto vpnErr = co_await vpnServer->Start();
    EXPECT_FALSE(vpnErr);

    auto vpnStopErr = co_await vpnServer->Stop();
    EXPECT_FALSE(vpnStopErr);

    co_await vpnServer->WaitService();

    co_await udpDynMux->Stop();
    co_await udpDynMux->WaitService();

    co_await tunSplit->Stop();

    auto& current = co_await Omni::Fiber::GetCurrentFiber();
    co_await current.WaitAll();

    ::close(externalFd);
    testPassed = true;
    co_return;
  });

  io.restart();
  io.run();
  EXPECT_TRUE(testPassed);
}

Packet CreateIPv6Packet(const boost::asio::ip::address_v6& src, const boost::asio::ip::address_v6& dest,
                        const std::string& payload) {
  std::size_t totalLen = 40 + payload.size();
  Packet p(totalLen);
  auto data = p.Data();
  std::fill(data.begin(), data.end(), 0);
  data[0] = 0x60; // Version 6

  uint16_t payloadLen = payload.size();
  data[4] = (payloadLen >> 8) & 0xFF;
  data[5] = payloadLen & 0xFF;

  auto srcBytes = src.to_bytes();
  std::copy(srcBytes.begin(), srcBytes.end(), data.begin() + 8);
  auto destBytes = dest.to_bytes();
  std::copy(destBytes.begin(), destBytes.end(), data.begin() + 24);
  std::copy(payload.begin(), payload.end(), data.begin() + 40);
  return p;
}

Packet CreateIPv4Packet(const boost::asio::ip::address_v4& src, const boost::asio::ip::address_v4& dest,
                        const std::string& payload) {
  std::size_t totalLen = 20 + payload.size();
  Packet p(totalLen);
  auto data = p.Data();
  std::fill(data.begin(), data.end(), 0);
  data[0] = 0x45; // Version 4, IHL 5

  // Total Length
  data[2] = (totalLen >> 8) & 0xFF;
  data[3] = totalLen & 0xFF;

  auto srcBytes = src.to_bytes();
  std::copy(srcBytes.begin(), srcBytes.end(), data.begin() + 12);
  auto destBytes = dest.to_bytes();
  std::copy(destBytes.begin(), destBytes.end(), data.begin() + 16);
  std::copy(payload.begin(), payload.end(), data.begin() + 20);
  return p;
}

TEST(VpnServerTest, EndToEndBidirectionalRouting) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  int fdsClient[2], fdsServer[2];
  ASSERT_GE(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fdsClient), 0);
  ASSERT_GE(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fdsServer), 0);

  auto tunClient = std::make_shared<EndpointTunSplitIp>(io.get_executor(), "client_tun", fdsClient[0]);
  auto tunServer = std::make_shared<EndpointTunSplitIp>(io.get_executor(), "server_tun", fdsServer[0]);
  auto udpClient = std::make_shared<UdpDynMux>(
      io.get_executor(), boost::asio::ip::udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  auto udpServer = std::make_shared<UdpDynMux>(
      io.get_executor(), boost::asio::ip::udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  auto vpnServer = std::make_shared<VpnServer>(tunServer, udpServer);

  UdpDynMux::PskType psk = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  auto clientIp = boost::asio::ip::make_address_v6("fd00::1");
  auto serverIp = boost::asio::ip::make_address_v6("fd00::2");
  auto clientIpV4 = boost::asio::ip::make_address_v4("10.0.0.1");
  auto serverIpV4 = boost::asio::ip::make_address_v4("10.0.0.2");

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto& current = co_await Omni::Fiber::GetCurrentFiber();

    co_await vpnServer->RegisterPeer(psk, {clientIp, MapToV6(clientIpV4)});

    // Start services
    EXPECT_FALSE(co_await tunClient->Start());
    EXPECT_FALSE(co_await tunServer->Start());
    EXPECT_FALSE(co_await udpClient->Start());
    EXPECT_FALSE(co_await udpServer->Start());
    EXPECT_FALSE(co_await vpnServer->Start());

    // Connect client
    auto resolver = std::make_shared<ResolverStaticEndpoint>(udpServer->LocalEndpoint());
    auto clientChannel = co_await udpClient->CreateChannel(psk, resolver);
    EXPECT_NE(clientChannel, nullptr);

    auto clientTunChannel = co_await tunClient->CreateChannel({serverIp, MapToV6(serverIpV4)});
    EXPECT_NE(clientTunChannel, nullptr);

    auto clientPipeline =
        std::make_shared<Pipeline>(clientTunChannel, std::vector<std::shared_ptr<Filter>>{}, clientChannel);
    EXPECT_FALSE(co_await clientPipeline->Start());

    // Wait for server notification / pipelines to set up
    boost::asio::steady_timer waitTimer(io.get_executor());
    waitTimer.expires_after(std::chrono::milliseconds(20));
    co_await waitTimer.async_wait(Omni::Fiber::AsioUseFiber);

    boost::asio::posix::stream_descriptor clientStack(io.get_executor(), fdsClient[1]);
    boost::asio::posix::stream_descriptor serverStack(io.get_executor(), fdsServer[1]);

    // Direction 1 (IPv6): Client -> Server
    {
      auto p1 = CreateIPv6Packet(clientIp, serverIp, "Hello Server!");
      auto [errWrite1, bytesWritten1] =
          co_await clientStack.async_write_some(boost::asio::const_buffer(p1), Omni::Fiber::AsioUseFiber);
      EXPECT_FALSE(errWrite1);

      Packet rxP1;
      auto [errRead1, bytesRead1] =
          co_await serverStack.async_read_some(boost::asio::mutable_buffer(rxP1), Omni::Fiber::AsioUseFiber);
      EXPECT_FALSE(errRead1);
      rxP1._Length = bytesRead1;
      EXPECT_GE(rxP1.DataSize(), 40);
      std::string payload1(reinterpret_cast<const char*>(rxP1.Data().data() + 40), rxP1.DataSize() - 40);
      EXPECT_EQ(payload1, "Hello Server!");
    }

    // Direction 2 (IPv6): Server -> Client
    {
      auto p2 = CreateIPv6Packet(serverIp, clientIp, "Hello Client!");
      auto [errWrite2, bytesWritten2] =
          co_await serverStack.async_write_some(boost::asio::const_buffer(p2), Omni::Fiber::AsioUseFiber);
      EXPECT_FALSE(errWrite2);

      Packet rxP2;
      auto [errRead2, bytesRead2] =
          co_await clientStack.async_read_some(boost::asio::mutable_buffer(rxP2), Omni::Fiber::AsioUseFiber);
      EXPECT_FALSE(errRead2);
      rxP2._Length = bytesRead2;
      EXPECT_GE(rxP2.DataSize(), 40);
      std::string payload2(reinterpret_cast<const char*>(rxP2.Data().data() + 40), rxP2.DataSize() - 40);
      EXPECT_EQ(payload2, "Hello Client!");
    }

    // Direction 1 (IPv4): Client -> Server
    {
      auto p1 = CreateIPv4Packet(clientIpV4, serverIpV4, "Hello Server V4!");
      auto [errWrite1, bytesWritten1] =
          co_await clientStack.async_write_some(boost::asio::const_buffer(p1), Omni::Fiber::AsioUseFiber);
      EXPECT_FALSE(errWrite1);

      Packet rxP1;
      auto [errRead1, bytesRead1] =
          co_await serverStack.async_read_some(boost::asio::mutable_buffer(rxP1), Omni::Fiber::AsioUseFiber);
      EXPECT_FALSE(errRead1);
      rxP1._Length = bytesRead1;
      EXPECT_GE(rxP1.DataSize(), 20);
      std::string payload1(reinterpret_cast<const char*>(rxP1.Data().data() + 20), rxP1.DataSize() - 20);
      EXPECT_EQ(payload1, "Hello Server V4!");
    }

    // Direction 2 (IPv4): Server -> Client
    {
      auto p2 = CreateIPv4Packet(serverIpV4, clientIpV4, "Hello Client V4!");
      auto [errWrite2, bytesWritten2] =
          co_await serverStack.async_write_some(boost::asio::const_buffer(p2), Omni::Fiber::AsioUseFiber);
      EXPECT_FALSE(errWrite2);

      Packet rxP2;
      auto [errRead2, bytesRead2] =
          co_await clientStack.async_read_some(boost::asio::mutable_buffer(rxP2), Omni::Fiber::AsioUseFiber);
      EXPECT_FALSE(errRead2);
      rxP2._Length = bytesRead2;
      EXPECT_GE(rxP2.DataSize(), 20);
      std::string payload2(reinterpret_cast<const char*>(rxP2.Data().data() + 20), rxP2.DataSize() - 20);
      EXPECT_EQ(payload2, "Hello Client V4!");
    }

    // Cleanup
    co_await clientPipeline->Stop();
    co_await vpnServer->Stop();
    co_await vpnServer->WaitService();
    co_await udpClient->Stop();
    co_await udpClient->WaitService();
    co_await udpServer->Stop();
    co_await udpServer->WaitService();
    co_await tunClient->Stop();
    co_await tunServer->Stop();

    co_await current.WaitAll();

    clientStack.close();
    serverStack.close();
    testPassed = true;
    co_return;
  });

  io.restart();
  io.run();
  EXPECT_TRUE(testPassed);
}
