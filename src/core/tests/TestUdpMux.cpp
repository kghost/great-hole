#include <memory>
#include <string>
#include <vector>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "EndpointUdpMux.hpp"
#include "Fiber.hpp"
#include "GetCurrentOmniFiber.hpp"
#include "Manager.hpp"
#include "Packet.hpp"
#include "ResolverHelper.hpp"

using boost::asio::ip::udp;
using namespace gh;

namespace {

void RunEventLoop(boost::asio::io_context& io) {
  io.restart();
  io.run();
}

} // namespace

TEST(UdpMuxTest, IgnoresStrayPackets) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto server = std::make_shared<UdpMux>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentOmniFiber();

    auto err = co_await server->Start();
    EXPECT_FALSE(err);
    if (err) {
      co_return;
    }

    auto channel = co_await server->CreateChannel(42);
    EXPECT_NE(channel, nullptr);
    if (channel == nullptr) {
      co_await server->Stop();
      co_return;
    }

    bool packetReceived = false;
    Cancel cancelObj;

    auto readFiber = current.Spawn("reader", [&]() -> Omni::Fiber::Coroutine<void> {
      Packet receivePacket;
      auto readErr = co_await channel->Read(receivePacket, cancelObj);
      EXPECT_FALSE(readErr);
      std::string receivedMsg(receivePacket._Data.begin() + receivePacket._Offset,
                              receivePacket._Data.begin() + receivePacket._Offset + receivePacket._Length);
      EXPECT_EQ(receivedMsg, "Hello");
      packetReceived = true;
      co_return;
    });

    // Create a plain raw UDP socket to send raw empty, wrong, and valid packets
    udp::socket rawSocket(io.get_executor(), udp::v6());

    // Send a 0-byte packet
    rawSocket.send_to(boost::asio::buffer("", 0), server->LocalEndpoint());

    // Send a packet with wrong channel ID (99)
    uint8_t wrongChannelPayload[] = {99, 'B', 'a', 'd'};
    rawSocket.send_to(boost::asio::buffer(wrongChannelPayload), server->LocalEndpoint());

    // Send a valid packet for channel 42
    uint8_t validPayload[] = {42, 'H', 'e', 'l', 'l', 'o'};
    rawSocket.send_to(boost::asio::buffer(validPayload), server->LocalEndpoint());

    co_await current.Join(readFiber);
    EXPECT_TRUE(packetReceived);

    co_await server->Stop();

    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(UdpMuxTest, ReadCancellation) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto server = std::make_shared<UdpMux>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentOmniFiber();

    auto err = co_await server->Start();
    EXPECT_FALSE(err);
    if (err) {
      co_return;
    }

    auto channel = co_await server->CreateChannel(42);
    EXPECT_NE(channel, nullptr);
    if (channel == nullptr) {
      co_await server->Stop();
      co_return;
    }

    Cancel cancelObj;
    bool readCompleted = false;
    ErrorCode readErrResult;

    auto readFiber = current.Spawn("reader", [&]() -> Omni::Fiber::Coroutine<void> {
      Packet receivePacket;
      readErrResult = co_await channel->Read(receivePacket, cancelObj);
      readCompleted = true;
      co_return;
    });

    auto interrupterFiber = current.Spawn("interrupter", [&]() -> Omni::Fiber::Coroutine<void> {
      cancelObj.Trigger();
      co_return;
    });

    co_await current.Join(readFiber);
    co_await current.Join(interrupterFiber);

    EXPECT_TRUE(readCompleted);
    EXPECT_EQ(readErrResult, Error(AppErrorCategory::kOperationAborted));

    co_await server->Stop();

    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(UdpMuxTest, WriteCancellation) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto server = std::make_shared<UdpMux>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentOmniFiber();

    auto err = co_await server->Start();
    EXPECT_FALSE(err);
    if (err) {
      co_return;
    }

    auto channel = co_await server->CreateChannel(42);
    EXPECT_NE(channel, nullptr);
    if (channel == nullptr) {
      co_await server->Stop();
      co_return;
    }

    Cancel cancelObj;
    cancelObj.Trigger(); // Cancel beforehand

    Packet sendPacket;
    std::string testMsg = "Cancel this write";
    std::copy(testMsg.begin(), testMsg.end(), sendPacket._Data.begin() + sendPacket._Offset);
    sendPacket._Length = testMsg.size();

    auto writeErr = co_await channel->Write(sendPacket, cancelObj);
    EXPECT_EQ(writeErr, Error(AppErrorCategory::kOperationAborted));

    co_await server->Stop();

    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(UdpMuxTest, DirectServerToServerMux) {
  class MockResolveFor : public ResolveFor {
  public:
    MockResolveFor(boost::asio::any_io_executor executor, std::string service, Protocol protocol)
        : _Executor(executor), _Service(service), _Protocol(protocol) {}
    ~MockResolveFor() override = default;

    auto GetExecutor() -> boost::asio::any_io_executor override { return _Executor; }
    auto GetService() -> std::string override { return _Service; }
    auto GetProtocol() -> Protocol override { return _Protocol; }

    boost::asio::any_io_executor _Executor;
    std::string _Service;
    Protocol _Protocol;
  };

  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto serverA = std::make_shared<UdpMux>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  auto serverB = std::make_shared<UdpMux>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentOmniFiber();

    auto errA = co_await serverA->Start();
    EXPECT_FALSE(errA);
    auto errB = co_await serverB->Start();
    EXPECT_FALSE(errB);
    if (errA || errB) {
      co_await serverA->Stop();
      co_await serverB->Stop();
      co_return;
    }

    auto mockedResolveFor = MockResolveFor(io.get_executor(), "", ResolveFor::Protocol::Udp);

    // Server A connects to Server B via resolver
    std::string resolverInput = "[::1]:" + std::to_string(serverB->LocalEndpoint().port());
    auto resolverToB = FindResolverEndpoint(resolverInput, mockedResolveFor);

    // Create active channel on Server A
    auto channelA = co_await serverA->CreateChannel(42, resolverToB);
    EXPECT_NE(channelA, nullptr);

    // Create passive channel on Server B
    auto channelB = co_await serverB->CreateChannel(42);
    EXPECT_NE(channelB, nullptr);

    if (channelA == nullptr || channelB == nullptr) {
      co_await serverA->Stop();
      co_await serverB->Stop();
      co_return;
    }

    // Ping test: Server A sends to Server B
    Packet sendPacket;
    std::string testMsg = "Direct Server-to-Server Mux!";
    std::copy(testMsg.begin(), testMsg.end(), sendPacket._Data.begin() + sendPacket._Offset);
    sendPacket._Length = testMsg.size();

    Cancel cancelObj;
    bool readBCompleted = false;
    bool writeACompleted = false;

    auto writeFiberA = current.Spawn("writerA", [&]() -> Omni::Fiber::Coroutine<void> {
      auto writeErr = co_await channelA->Write(sendPacket, cancelObj);
      EXPECT_FALSE(writeErr);
      writeACompleted = true;
      co_return;
    });

    auto readFiberB = current.Spawn("readerB", [&]() -> Omni::Fiber::Coroutine<void> {
      Packet receivePacket;
      auto readErr = co_await channelB->Read(receivePacket, cancelObj);
      EXPECT_FALSE(readErr);

      std::string receivedMsg(receivePacket._Data.begin() + receivePacket._Offset,
                              receivePacket._Data.begin() + receivePacket._Offset + receivePacket._Length);
      EXPECT_EQ(receivedMsg, testMsg);
      readBCompleted = true;
      co_return;
    });

    co_await current.Join(writeFiberA);
    co_await current.Join(readFiberB);

    EXPECT_TRUE(readBCompleted);
    EXPECT_TRUE(writeACompleted);

    // Pong test: Server B now has learned the peer endpoint, it sends a reply to Server A
    Packet replyPacket;
    std::string replyMsg = "Direct Server-to-Server Mux Reply!";
    std::copy(replyMsg.begin(), replyMsg.end(), replyPacket._Data.begin() + replyPacket._Offset);
    replyPacket._Length = replyMsg.size();

    bool readACompleted = false;
    bool writeBCompleted = false;

    auto writeFiberB = current.Spawn("writerB", [&]() -> Omni::Fiber::Coroutine<void> {
      auto writeErr = co_await channelB->Write(replyPacket, cancelObj);
      EXPECT_FALSE(writeErr);
      writeBCompleted = true;
      co_return;
    });

    auto readFiberA = current.Spawn("readerA", [&]() -> Omni::Fiber::Coroutine<void> {
      Packet receivePacket;
      auto readErr = co_await channelA->Read(receivePacket, cancelObj);
      EXPECT_FALSE(readErr);

      std::string receivedMsg(receivePacket._Data.begin() + receivePacket._Offset,
                              receivePacket._Data.begin() + receivePacket._Offset + receivePacket._Length);
      EXPECT_EQ(receivedMsg, replyMsg);
      readACompleted = true;
      co_return;
    });

    co_await current.Join(writeFiberB);
    co_await current.Join(readFiberA);

    EXPECT_TRUE(readACompleted);
    EXPECT_TRUE(writeBCompleted);

    co_await serverA->Stop();
    co_await serverB->Stop();

    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}
