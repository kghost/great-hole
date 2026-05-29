#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "EndpointUdpDynMuxClient.hpp"
#include "EndpointUdpDynMuxServer.hpp"
#include "EndpointUdpDynMuxProtocol.hpp"
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

TEST(UdpDynMuxFiberTest, SuccessfulChannelCreationAndDataTransfer) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  auto server = std::make_shared<UdpDynMuxServer>(io, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentFiber();

    auto err = co_await server->Start();
    EXPECT_FALSE(err);
    if (err) {
      co_return;
    }

    auto serverEp = server->LocalEndpoint();

    auto client = std::make_shared<UdpDynMuxClient>(io, serverEp, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
    auto clientErr = co_await client->Start();
    EXPECT_FALSE(clientErr);
    if (clientErr) {
      co_await server->Stop();
      co_return;
    }

    // Server dynamically maps channel ID 1 (which will be negotiated)
    auto channel = co_await server->CreateChannel(1);
    EXPECT_NE(channel, nullptr);
    if (channel == nullptr) {
      co_await client->Stop();
      co_await server->Stop();
      co_return;
    }

    // Prepare packet to send from client to server channel 1
    Packet sendPacket;
    std::string testMsg = "Hello Dyn Mux Fiber!";
    // Make sure we have space for 2-byte ID
    sendPacket._Offset = 2;
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

    // Now test write from server channel 1 back to client
    Packet replyPacket;
    std::string replyMsg = "Reply Dyn Mux Fiber!";
    replyPacket._Offset = 2;
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

TEST(UdpDynMuxFiberTest, PacketQueueingDuringNegotiation) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  auto server = std::make_shared<UdpDynMuxServer>(io, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentFiber();

    auto err = co_await server->Start();
    EXPECT_FALSE(err);
    if (err) {
      co_return;
    }

    auto serverEp = server->LocalEndpoint();

    auto client = std::make_shared<UdpDynMuxClient>(io, serverEp, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
    
    // Create server dynamic channel
    auto channel = co_await server->CreateChannel(1);
    EXPECT_NE(channel, nullptr);

    // Call Start on client, which triggers negotiation loop in background
    auto clientErr = co_await client->Start();
    EXPECT_FALSE(clientErr);

    // Write a packet immediately (will be queued / suspended while client negotiates)
    Packet sendPacket;
    std::string testMsg = "Queued Dynamic!";
    sendPacket._Offset = 2;
    std::copy(testMsg.begin(), testMsg.end(), sendPacket._Data.begin() + sendPacket._Offset);
    sendPacket._Length = testMsg.size();

    Cancel cancelObj;
    bool writeCompleted = false;
    bool readCompleted = false;

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

    EXPECT_TRUE(writeCompleted);
    EXPECT_TRUE(readCompleted);

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

TEST(UdpDynMuxFiberTest, ServerHandlesMigration) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  auto server = std::make_shared<UdpDynMuxServer>(io, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentFiber();

    auto err = co_await server->Start();
    EXPECT_FALSE(err);
    if (err) {
      co_return;
    }

    auto serverEp = server->LocalEndpoint();
    auto channel = co_await server->CreateChannel(1);
    EXPECT_NE(channel, nullptr);

    // Mock client socket 1
    udp::socket client1(io, udp::endpoint(udp::v6(), 0));
    uint32_t cookie = 0x12345678;

    uint8_t txBuf[2048];
    UdpDynMux::ClientReqId{cookie}.Serialize(txBuf);

    // Send negotiation
    auto [sendErr1, n1] = co_await client1.async_send_to(
        boost::asio::buffer(txBuf, UdpDynMux::ClientReqId::kSize), serverEp, Omni::Fiber::AsioUseFiber);
    EXPECT_FALSE(sendErr1);

    // Receive assignment on client 1
    uint8_t rxBuf[2048];
    udp::endpoint senderEp;
    auto [recvErr1, n2] = co_await client1.async_receive_from(
        boost::asio::mutable_buffer(rxBuf, sizeof(rxBuf)), senderEp, Omni::Fiber::AsioUseFiber);
    EXPECT_FALSE(recvErr1);

    auto assign = UdpDynMux::ServerAssignId::Deserialize(rxBuf, n2);
    EXPECT_TRUE(assign.has_value());
    if (!assign.has_value()) {
      co_await server->Stop();
      co_return;
    }
    EXPECT_EQ(assign->Cookie, cookie);
    uint16_t id = assign->AssignedId;
    EXPECT_EQ(id, 1);

    // Now migrate to client socket 2
    udp::socket client2(io, udp::endpoint(udp::v6(), 0));
    UdpDynMux::ClientAddrMigrate{id, cookie}.Serialize(txBuf);

    auto [sendErr2, n3] = co_await client2.async_send_to(
        boost::asio::buffer(txBuf, UdpDynMux::ClientAddrMigrate::kSize), serverEp, Omni::Fiber::AsioUseFiber);
    EXPECT_FALSE(sendErr2);

    // Receive migration ACK on client 2
    auto [recvErr2, n4] = co_await client2.async_receive_from(
        boost::asio::mutable_buffer(rxBuf, sizeof(rxBuf)), senderEp, Omni::Fiber::AsioUseFiber);
    EXPECT_FALSE(recvErr2);

    auto migrateAck = UdpDynMux::ServerMigrateAck::Deserialize(rxBuf, n4);
    EXPECT_TRUE(migrateAck.has_value());
    if (!migrateAck.has_value()) {
      co_await server->Stop();
      co_return;
    }
    EXPECT_EQ(migrateAck->Id, id);

    auto stopErr = co_await server->Stop();
    EXPECT_FALSE(stopErr);

    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(UdpDynMuxFiberTest, ReadCancellation) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  auto server = std::make_shared<UdpDynMuxServer>(io, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentFiber();

    auto err = co_await server->Start();
    EXPECT_FALSE(err);
    if (err) {
      co_return;
    }

    auto channel = co_await server->CreateChannel(1);
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

TEST(UdpDynMuxFiberTest, WriteCancellation) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  auto server = std::make_shared<UdpDynMuxServer>(io, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentFiber();

    auto err = co_await server->Start();
    EXPECT_FALSE(err);
    if (err) {
      co_return;
    }

    auto channel = co_await server->CreateChannel(1);
    EXPECT_NE(channel, nullptr);
    if (channel == nullptr) {
      co_await server->Stop();
      co_return;
    }

    Cancel cancelObj;
    cancelObj.Trigger(); // Cancel beforehand

    Packet sendPacket;
    std::string testMsg = "Cancel dynamic write";
    sendPacket._Offset = 2;
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
