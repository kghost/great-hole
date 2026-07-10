#include <memory>
#include <string>
#include <vector>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "EndpointUdp.hpp"
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

TEST(UdpFiberTest, SuccessfulChannelCreationAndDataTransfer) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto udp1 = std::make_shared<Udp>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  auto udp2 = std::make_shared<Udp>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentOmniFiber();

    // Start both endpoints
    auto err1 = co_await udp1->Start();
    EXPECT_FALSE(err1);
    if (err1) {
      co_return;
    }
    auto err2 = co_await udp2->Start();
    EXPECT_FALSE(err2);
    if (err2) {
      co_return;
    }

    // Get bound local endpoints
    auto ep1 = udp1->LocalEndpoint();
    auto ep2 = udp2->LocalEndpoint();

    // Create channels asynchronously within the fiber context
    auto channel1 = co_await udp1->CreateChannel(ep2);
    EXPECT_NE(channel1, nullptr);
    if (channel1 == nullptr) {
      co_return;
    }

    auto channel2 = co_await udp2->CreateChannel(ep1);
    EXPECT_NE(channel2, nullptr);
    if (channel2 == nullptr) {
      co_return;
    }

    // Prepare packet to send from channel1 to channel2
    Packet sendPacket;
    std::string testMsg = "Hello, UDP Fiber Channel!";
    std::copy(testMsg.begin(), testMsg.end(), sendPacket._Data.begin() + sendPacket._Offset);
    sendPacket._Length = testMsg.size();

    Cancel cancelObj;

    // Run write and read concurrently by spawning child fibers
    bool readCompleted = false;
    bool writeCompleted = false;

    auto writeFiber = current.Spawn("writer", [&]() -> Omni::Fiber::Coroutine<void> {
      auto writeErr = co_await channel1->Write(sendPacket, cancelObj);
      EXPECT_FALSE(writeErr);
      writeCompleted = true;
      co_return;
    });

    auto readFiber = current.Spawn("reader", [&]() -> Omni::Fiber::Coroutine<void> {
      Packet receivePacket;
      auto readErr = co_await channel2->Read(receivePacket, cancelObj);
      EXPECT_FALSE(readErr);

      std::string receivedMsg(receivePacket._Data.begin() + receivePacket._Offset,
                              receivePacket._Data.begin() + receivePacket._Offset + receivePacket._Length);
      EXPECT_EQ(receivedMsg, testMsg);
      readCompleted = true;
      co_return;
    });

    // Join read/write fibers
    co_await current.Join(writeFiber);
    co_await current.Join(readFiber);

    EXPECT_TRUE(readCompleted);
    EXPECT_TRUE(writeCompleted);

    // Stop endpoints gracefully
    co_await udp1->Stop();
    co_await udp2->Stop();

    // Reap all child fibers spawned on the root fiber (i.e. start fibers)
    co_await current.WaitAll();

    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(UdpFiberTest, StartFailureOnDuplicateBind) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto udp1 = std::make_shared<Udp>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentOmniFiber();

    auto err1 = co_await udp1->Start();
    EXPECT_FALSE(err1);
    if (err1) {
      co_return;
    }

    auto ep1 = udp1->LocalEndpoint();
    auto udp2 = std::make_shared<Udp>(io.get_executor(), ep1);

    auto err2 = co_await udp2->Start();
    EXPECT_TRUE(err2); // Duplicate bind should return error

    co_await udp1->Stop();

    co_await current.WaitAll();

    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(UdpFiberTest, ReadCancellation) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto udp1 = std::make_shared<Udp>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  auto udp2 = std::make_shared<Udp>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentOmniFiber();

    auto err1 = co_await udp1->Start();
    EXPECT_FALSE(err1);
    auto err2 = co_await udp2->Start();
    EXPECT_FALSE(err2);

    auto ep1 = udp1->LocalEndpoint();
    auto ep2 = udp2->LocalEndpoint();

    auto channel1 = co_await udp1->CreateChannel(ep2);
    EXPECT_NE(channel1, nullptr);
    if (channel1 == nullptr) {
      co_return;
    }

    auto channel2 = co_await udp2->CreateChannel(ep1);
    EXPECT_NE(channel2, nullptr);
    if (channel2 == nullptr) {
      co_return;
    }

    Cancel cancelObj;
    bool readCompleted = false;
    ErrorCode readErrResult;

    auto readFiber = current.Spawn("reader", [&]() -> Omni::Fiber::Coroutine<void> {
      Packet receivePacket;
      readErrResult = co_await channel2->Read(receivePacket, cancelObj);
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

    co_await udp1->Stop();
    co_await udp2->Stop();

    co_await current.WaitAll();

    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(UdpFiberTest, WriteCancellation) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto udp1 = std::make_shared<Udp>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  auto udp2 = std::make_shared<Udp>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentOmniFiber();

    auto err1 = co_await udp1->Start();
    EXPECT_FALSE(err1);
    auto err2 = co_await udp2->Start();
    EXPECT_FALSE(err2);

    auto ep1 = udp1->LocalEndpoint();
    auto ep2 = udp2->LocalEndpoint();

    auto channel1 = co_await udp1->CreateChannel(ep2);
    EXPECT_NE(channel1, nullptr);
    if (channel1 == nullptr) {
      co_return;
    }

    auto channel2 = co_await udp2->CreateChannel(ep1);
    EXPECT_NE(channel2, nullptr);
    if (channel2 == nullptr) {
      co_return;
    }

    Cancel cancelObj;
    cancelObj.Trigger(); // Cancel beforehand

    Packet sendPacket;
    std::string testMsg = "Cancel this write";
    std::copy(testMsg.begin(), testMsg.end(), sendPacket._Data.begin() + sendPacket._Offset);
    sendPacket._Length = testMsg.size();

    auto writeErr = co_await channel1->Write(sendPacket, cancelObj);
    EXPECT_EQ(writeErr, ErrorCode(AppErrorCategory::kOperationAborted, kAppError));

    co_await udp1->Stop();
    co_await udp2->Stop();

    co_await current.WaitAll();

    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(UdpFiberTest, MultiplePacketsPingPong) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto udp1 = std::make_shared<Udp>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  auto udp2 = std::make_shared<Udp>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentOmniFiber();

    auto err1 = co_await udp1->Start();
    EXPECT_FALSE(err1);
    auto err2 = co_await udp2->Start();
    EXPECT_FALSE(err2);

    auto ep1 = udp1->LocalEndpoint();
    auto ep2 = udp2->LocalEndpoint();

    auto channel1 = co_await udp1->CreateChannel(ep2);
    EXPECT_NE(channel1, nullptr);
    if (channel1 == nullptr) {
      co_return;
    }

    auto channel2 = co_await udp2->CreateChannel(ep1);
    EXPECT_NE(channel2, nullptr);
    if (channel2 == nullptr) {
      co_return;
    }

    Cancel cancelObj;
    constexpr int kRounds = 5;
    bool clientPassed = false;
    bool serverPassed = false;

    // Server Fiber: receives Ping, replies Pong
    auto serverFiber = current.Spawn("server", [&]() -> Omni::Fiber::Coroutine<void> {
      for (int round = 0; round < kRounds; ++round) {
        Packet receivePacket;
        auto readErr = co_await channel2->Read(receivePacket, cancelObj);
        EXPECT_FALSE(readErr);
        if (readErr) {
          co_return;
        }

        std::string receivedMsg(receivePacket._Data.begin() + receivePacket._Offset,
                                receivePacket._Data.begin() + receivePacket._Offset + receivePacket._Length);
        EXPECT_EQ(receivedMsg, "Ping " + std::to_string(round));

        Packet replyPacket;
        std::string replyMsg = "Pong " + std::to_string(round);
        std::copy(replyMsg.begin(), replyMsg.end(), replyPacket._Data.begin() + replyPacket._Offset);
        replyPacket._Length = replyMsg.size();

        auto writeErr = co_await channel2->Write(replyPacket, cancelObj);
        EXPECT_FALSE(writeErr);
      }
      serverPassed = true;
      co_return;
    });

    // Client Fiber: sends Ping, receives Pong
    auto clientFiber = current.Spawn("client", [&]() -> Omni::Fiber::Coroutine<void> {
      for (int round = 0; round < kRounds; ++round) {
        Packet sendPacket;
        std::string testMsg = "Ping " + std::to_string(round);
        std::copy(testMsg.begin(), testMsg.end(), sendPacket._Data.begin() + sendPacket._Offset);
        sendPacket._Length = testMsg.size();

        auto writeErr = co_await channel1->Write(sendPacket, cancelObj);
        EXPECT_FALSE(writeErr);
        if (writeErr) {
          co_return;
        }

        Packet receivePacket;
        auto readErr = co_await channel1->Read(receivePacket, cancelObj);
        EXPECT_FALSE(readErr);
        if (readErr) {
          co_return;
        }

        std::string receivedMsg(receivePacket._Data.begin() + receivePacket._Offset,
                                receivePacket._Data.begin() + receivePacket._Offset + receivePacket._Length);
        EXPECT_EQ(receivedMsg, "Pong " + std::to_string(round));
      }
      clientPassed = true;
      co_return;
    });

    co_await current.Join(serverFiber);
    co_await current.Join(clientFiber);

    EXPECT_TRUE(serverPassed);
    EXPECT_TRUE(clientPassed);

    co_await udp1->Stop();
    co_await udp2->Stop();

    co_await current.WaitAll();

    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(UdpFiberTest, CreateChannelFromResolverEndpoint) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto udp1 = std::make_shared<Udp>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  auto udp2 = std::make_shared<Udp>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentOmniFiber();

    // Start both endpoints
    auto err1 = co_await udp1->Start();
    EXPECT_FALSE(err1);
    if (err1) {
      co_return;
    }
    auto err2 = co_await udp2->Start();
    EXPECT_FALSE(err2);
    if (err2) {
      co_return;
    }

    // Get bound local endpoints
    auto ep1 = udp1->LocalEndpoint();
    auto ep2 = udp2->LocalEndpoint();

    class MockResolveeFor : public ResolveFor {
    public:
      MockResolveeFor(boost::asio::any_io_executor executor, std::string service, Protocol protocol)
          : _Executor(executor), _Service(service), _Protocol(protocol) {}
      ~MockResolveeFor() override = default;

      auto GetExecutor() -> boost::asio::any_io_executor override { return _Executor; }
      auto GetService() -> std::string override { return _Service; }
      auto GetProtocol() -> Protocol override { return _Protocol; }

      boost::asio::any_io_executor _Executor;
      std::string _Service;
      Protocol _Protocol;
    };
    auto mockedResolveFor = MockResolveeFor(io.get_executor(), "", ResolveFor::Protocol::Udp);

    // Create a dynamic ResolverEndpoint for the peer
    std::string resolverInput = "[::1]:" + std::to_string(ep2.port());
    auto resolver = FindResolverEndpoint(resolverInput, mockedResolveFor);

    // Create a channel using the ResolverEndpoint
    auto channel1 = co_await udp1->CreateChannel(resolver);
    EXPECT_NE(channel1, nullptr);
    if (channel1 == nullptr) {
      co_return;
    }

    // Create channel2 normally
    auto channel2 = co_await udp2->CreateChannel(ep1);
    EXPECT_NE(channel2, nullptr);
    if (channel2 == nullptr) {
      co_return;
    }

    // Ping pong test
    Packet sendPacket;
    std::string testMsg = "Hello Dynamic Resolver!";
    std::copy(testMsg.begin(), testMsg.end(), sendPacket._Data.begin() + sendPacket._Offset);
    sendPacket._Length = testMsg.size();

    Cancel cancelObj;
    bool readCompleted = false;
    bool writeCompleted = false;

    auto writeFiber = current.Spawn("writer", [&]() -> Omni::Fiber::Coroutine<void> {
      auto writeErr = co_await channel1->Write(sendPacket, cancelObj);
      EXPECT_FALSE(writeErr);
      writeCompleted = true;
      co_return;
    });

    auto readFiber = current.Spawn("reader", [&]() -> Omni::Fiber::Coroutine<void> {
      Packet receivePacket;
      auto readErr = co_await channel2->Read(receivePacket, cancelObj);
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

    co_await udp1->Stop();
    co_await udp2->Stop();

    co_await current.WaitAll();

    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}
