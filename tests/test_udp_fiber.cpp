#include <memory>
#include <string>
#include <vector>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "Fiber.hpp"
#include "GetCurrentFiber.hpp"
#include "Manager.hpp"
#include "endpoint-udp.hpp"
#include "packet.hpp"

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
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  auto udp1 = std::make_shared<Udp>(io, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  auto udp2 = std::make_shared<Udp>(io, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentFiber();

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
    auto stopErr1 = co_await udp1->Stop();
    EXPECT_FALSE(stopErr1);
    auto stopErr2 = co_await udp2->Stop();
    EXPECT_FALSE(stopErr2);

    // Reap all child fibers spawned on the root fiber (i.e. start fibers)
    co_await current.WaitAll();

    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}
