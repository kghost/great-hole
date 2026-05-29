#include <memory>
#include <string>
#include <vector>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "EndpointUdpMuxClient.hpp"
#include "EndpointUdpMuxServer.hpp"
#include "Fiber.hpp"
#include "GetCurrentFiber.hpp"
#include "Manager.hpp"
#include "Packet.hpp"

using boost::asio::ip::udp;
using namespace gh;

namespace {

void RunEventLoop(boost::asio::io_context& io) {
  io.restart();
  io.run();
}

} // namespace

TEST(UdpMuxFiberTest, SuccessfulChannelCreationAndDataTransfer) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  auto server = std::make_shared<UdpMuxServer>(io, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentFiber();

    auto err = co_await server->Start();
    EXPECT_FALSE(err);
    if (err) {
      co_return;
    }

    auto serverEp = server->LocalEndpoint();

    auto client = std::make_shared<UdpMuxClient>(io, 42, serverEp, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
    auto clientErr = co_await client->Start();
    EXPECT_FALSE(clientErr);
    if (clientErr) {
      co_await server->Stop();
      co_return;
    }

    auto channel = co_await server->CreateChannel(42);
    EXPECT_NE(channel, nullptr);
    if (channel == nullptr) {
      co_await client->Stop();
      co_await server->Stop();
      co_return;
    }

    // Prepare packet to send from client to server channel 42
    Packet sendPacket;
    std::string testMsg = "Hello Mux Fiber!";
    std::copy(testMsg.begin(), testMsg.end(), sendPacket._Data.begin() + sendPacket._Offset);
    sendPacket._Length = testMsg.size();

    Cancel cancelObj;
    bool readCompleted = false;
    bool writeCompleted = false;

    auto writeFiber = current.Spawn("writer", [&]() -> Omni::Fiber::Coroutine<void> {
      auto writeErr = co_await client->Write(sendPacket, cancelObj);
      EXPECT_FALSE(writeErr);
      writeCompleted = true;
      co_return;
    });

    auto readFiber = current.Spawn("reader", [&]() -> Omni::Fiber::Coroutine<void> {
      Packet receivePacket;
      auto readErr = co_await channel->Read(receivePacket, cancelObj);
      EXPECT_FALSE(readErr);

      std::string receivedMsg(receivePacket._Data.begin() + receivePacket._Offset,
                              receivePacket._Data.begin() + receivePacket._Offset + receivePacket._Length);
      EXPECT_EQ(receivedMsg, testMsg);
      readCompleted = true;
      co_return;
    });

    co_await current.Join(writeFiber);
    co_await current.Join(readFiber);

    EXPECT_TRUE(readCompleted);
    EXPECT_TRUE(writeCompleted);

    // Now test write from server channel 42 to client
    Packet replyPacket;
    std::string replyMsg = "Reply Mux Fiber!";
    std::copy(replyMsg.begin(), replyMsg.end(), replyPacket._Data.begin() + replyPacket._Offset);
    replyPacket._Length = replyMsg.size();

    bool replyReadCompleted = false;
    bool replyWriteCompleted = false;

    auto replyWriteFiber = current.Spawn("reply_writer", [&]() -> Omni::Fiber::Coroutine<void> {
      auto writeErr = co_await channel->Write(replyPacket, cancelObj);
      EXPECT_FALSE(writeErr);
      replyWriteCompleted = true;
      co_return;
    });

    auto replyReadFiber = current.Spawn("reply_reader", [&]() -> Omni::Fiber::Coroutine<void> {
      Packet receivePacket;
      auto readErr = co_await client->Read(receivePacket, cancelObj);
      EXPECT_FALSE(readErr);

      std::string receivedMsg(receivePacket._Data.begin() + receivePacket._Offset,
                              receivePacket._Data.begin() + receivePacket._Offset + receivePacket._Length);
      EXPECT_EQ(receivedMsg, replyMsg);
      replyReadCompleted = true;
      co_return;
    });

    co_await current.Join(replyWriteFiber);
    co_await current.Join(replyReadFiber);

    EXPECT_TRUE(replyReadCompleted);
    EXPECT_TRUE(replyWriteCompleted);

    auto stopErr1 = co_await client->Stop();
    EXPECT_FALSE(stopErr1);
    auto stopErr2 = co_await server->Stop();
    EXPECT_FALSE(stopErr2);

    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(UdpMuxFiberTest, IgnoresEmptyPacket) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  auto server = std::make_shared<UdpMuxServer>(io, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentFiber();

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

    // Create a plain raw UDP socket to send raw empty and valid packets
    udp::socket rawSocket(io, udp::v6());

    // Send a 0-byte packet
    rawSocket.send_to(boost::asio::buffer("", 0), server->LocalEndpoint());

    // Send a valid packet for channel 42
    uint8_t validPayload[] = {42, 'H', 'e', 'l', 'l', 'o'};
    rawSocket.send_to(boost::asio::buffer(validPayload), server->LocalEndpoint());

    co_await current.Join(readFiber);
    EXPECT_TRUE(packetReceived);

    auto stopErr = co_await server->Stop();
    EXPECT_FALSE(stopErr);

    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(UdpMuxFiberTest, IgnoresStrayPackets) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  // Setup a plain raw UDP socket to act as peer
  udp::socket rawSocket(io, udp::v6());
  rawSocket.bind(udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  udp::endpoint peerEp = rawSocket.local_endpoint();

  auto client = std::make_shared<UdpMuxClient>(io, 42, peerEp, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentFiber();

    auto err = co_await client->Start();
    EXPECT_FALSE(err);
    if (err) {
      co_return;
    }

    bool packetReceived = false;
    Cancel cancelObj;

    auto readFiber = current.Spawn("reader", [&]() -> Omni::Fiber::Coroutine<void> {
      Packet receivePacket;
      auto readErr = co_await client->Read(receivePacket, cancelObj);
      EXPECT_FALSE(readErr);
      std::string receivedMsg(receivePacket._Data.begin() + receivePacket._Offset,
                              receivePacket._Data.begin() + receivePacket._Offset + receivePacket._Length);
      EXPECT_EQ(receivedMsg, "Hello");
      packetReceived = true;
      co_return;
    });

    rawSocket.connect(client->LocalEndpoint());

    // Send a 0-byte packet
    rawSocket.send(boost::asio::buffer("", 0));

    // Send a packet with wrong channel ID (99)
    uint8_t wrongChannelPayload[] = {99, 'B', 'a', 'd'};
    rawSocket.send(boost::asio::buffer(wrongChannelPayload));

    // Send a valid packet for channel 42
    uint8_t validPayload[] = {42, 'H', 'e', 'l', 'l', 'o'};
    rawSocket.send(boost::asio::buffer(validPayload));

    co_await current.Join(readFiber);
    EXPECT_TRUE(packetReceived);

    auto stopErr = co_await client->Stop();
    EXPECT_FALSE(stopErr);

    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(UdpMuxFiberTest, ReadCancellation) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  auto server = std::make_shared<UdpMuxServer>(io, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentFiber();

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
    EXPECT_EQ(readErrResult, ErrorCode(AppErrorCategory::kOperationAborted, kAppError));

    auto stopErr = co_await server->Stop();
    EXPECT_FALSE(stopErr);

    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(UdpMuxFiberTest, WriteCancellation) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  auto server = std::make_shared<UdpMuxServer>(io, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentFiber();

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
    EXPECT_EQ(writeErr, ErrorCode(AppErrorCategory::kOperationAborted, kAppError));

    auto stopErr = co_await server->Stop();
    EXPECT_FALSE(stopErr);

    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}
