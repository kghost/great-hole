#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "EndpointUdpDynMux.hpp"
#include "Fiber.hpp"
#include "GetCurrentFiber.hpp"
#include "Manager.hpp"
#include "Packet.hpp"
#include "ResolverStaticEndpoint.hpp"
#include "Yield.hpp"

using boost::asio::ip::udp;
using namespace gh;

namespace {

void RunEventLoop(boost::asio::io_context& io) {
  io.restart();
  io.run();
}

} // namespace

TEST(UdpDynMuxTest, SuccessfulChannelCreationAndDataTransfer) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto dev1 = std::make_shared<UdpDynMux>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  auto dev2 = std::make_shared<UdpDynMux>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentFiber();

    auto err1 = co_await dev1->Start();
    EXPECT_FALSE(err1);
    auto err2 = co_await dev2->Start();
    EXPECT_FALSE(err2);

    UdpDynMux::PskType psk = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

    auto ch1 = co_await dev1->CreateChannel(psk);
    EXPECT_NE(ch1, nullptr);

    auto resolver = std::make_shared<ResolverStaticEndpoint>(dev1->LocalEndpoint());
    auto ch2 = co_await dev2->CreateChannel(psk, resolver);
    EXPECT_NE(ch2, nullptr);

    Cancel cancelObj;
    co_await Omni::Fiber::Yield();
    boost::asio::steady_timer waitTimer(io.get_executor());
    waitTimer.expires_after(std::chrono::milliseconds(20));
    co_await waitTimer.async_wait(Omni::Fiber::AsioUseFiber);
    co_await Omni::Fiber::Yield();

    Packet p1;
    std::string msg1 = "Hello Symmetric Mux!";
    p1._Offset = 2;
    std::copy(msg1.begin(), msg1.end(), p1._Data.begin() + p1._Offset);
    p1._Length = msg1.size();

    bool writeCompleted = false;
    bool readCompleted = false;

    auto writeFiber = current.Spawn("writer", [&]() -> Omni::Fiber::Coroutine<void> {
      auto writeErr = co_await ch2->Write(p1, cancelObj);
      EXPECT_FALSE(writeErr);
      writeCompleted = true;
      co_return;
    });

    auto readFiber = current.Spawn("reader", [&]() -> Omni::Fiber::Coroutine<void> {
      Packet rxP;
      auto readErr = co_await ch1->Read(rxP, cancelObj);
      EXPECT_FALSE(readErr);
      std::string rxMsg(rxP._Data.begin() + rxP._Offset, rxP._Data.begin() + rxP._Offset + rxP._Length);
      EXPECT_EQ(rxMsg, msg1);
      readCompleted = true;
      co_return;
    });

    co_await current.Join(writeFiber);
    co_await current.Join(readFiber);

    EXPECT_TRUE(writeCompleted);
    EXPECT_TRUE(readCompleted);

    Packet p2;
    std::string msg2 = "Reply Symmetric!";
    p2._Offset = 2;
    std::copy(msg2.begin(), msg2.end(), p2._Data.begin() + p2._Offset);
    p2._Length = msg2.size();

    bool replyWriteCompleted = false;
    bool replyReadCompleted = false;

    auto replyWriteFiber = current.Spawn("reply_writer", [&]() -> Omni::Fiber::Coroutine<void> {
      auto writeErr = co_await ch1->Write(p2, cancelObj);
      EXPECT_FALSE(writeErr);
      replyWriteCompleted = true;
      co_return;
    });

    auto replyReadFiber = current.Spawn("reply_reader", [&]() -> Omni::Fiber::Coroutine<void> {
      Packet rxP;
      auto readErr = co_await ch2->Read(rxP, cancelObj);
      EXPECT_FALSE(readErr);
      std::string rxMsg(rxP._Data.begin() + rxP._Offset, rxP._Data.begin() + rxP._Offset + rxP._Length);
      EXPECT_EQ(rxMsg, msg2);
      replyReadCompleted = true;
      co_return;
    });

    co_await current.Join(replyWriteFiber);
    co_await current.Join(replyReadFiber);

    EXPECT_TRUE(replyWriteCompleted);
    EXPECT_TRUE(replyReadCompleted);

    co_await dev1->Stop();
    co_await dev2->Stop();

    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(UdpDynMuxTest, SimultaneousConnectionStart) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto dev1 = std::make_shared<UdpDynMux>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  auto dev2 = std::make_shared<UdpDynMux>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentFiber();

    auto err1 = co_await dev1->Start();
    EXPECT_FALSE(err1);
    auto err2 = co_await dev2->Start();
    EXPECT_FALSE(err2);

    UdpDynMux::PskType psk = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};

    auto res1 = std::make_shared<ResolverStaticEndpoint>(dev2->LocalEndpoint());
    auto res2 = std::make_shared<ResolverStaticEndpoint>(dev1->LocalEndpoint());

    auto ch1 = co_await dev1->CreateChannel(psk, res1);
    auto ch2 = co_await dev2->CreateChannel(psk, res2);

    EXPECT_NE(ch1, nullptr);
    EXPECT_NE(ch2, nullptr);

    Cancel cancelObj;
    co_await Omni::Fiber::Yield();
    boost::asio::steady_timer timer(io.get_executor());
    timer.expires_after(std::chrono::milliseconds(20));
    co_await timer.async_wait(Omni::Fiber::AsioUseFiber);
    co_await Omni::Fiber::Yield();

    Packet p1;
    std::string msg1 = "Simultaneous!";
    p1._Offset = 2;
    std::copy(msg1.begin(), msg1.end(), p1._Data.begin() + p1._Offset);
    p1._Length = msg1.size();

    bool writeCompleted = false;
    bool readCompleted = false;

    auto writeFiber = current.Spawn("writer", [&]() -> Omni::Fiber::Coroutine<void> {
      auto writeErr = co_await ch1->Write(p1, cancelObj);
      EXPECT_FALSE(writeErr);
      writeCompleted = true;
      co_return;
    });

    auto readFiber = current.Spawn("reader", [&]() -> Omni::Fiber::Coroutine<void> {
      Packet rxP;
      auto readErr = co_await ch2->Read(rxP, cancelObj);
      EXPECT_FALSE(readErr);
      std::string rxMsg(rxP._Data.begin() + rxP._Offset, rxP._Data.begin() + rxP._Offset + rxP._Length);
      EXPECT_EQ(rxMsg, msg1);
      readCompleted = true;
      co_return;
    });

    co_await current.Join(writeFiber);
    co_await current.Join(readFiber);

    EXPECT_TRUE(writeCompleted);
    EXPECT_TRUE(readCompleted);

    co_await dev1->Stop();
    co_await dev2->Stop();

    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(UdpDynMuxTest, AddressMigration) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto dev1 = std::make_shared<UdpDynMux>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  auto dev2 = std::make_shared<UdpDynMux>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentFiber();

    auto err1 = co_await dev1->Start();
    EXPECT_FALSE(err1);
    auto err2 = co_await dev2->Start();
    EXPECT_FALSE(err2);

    UdpDynMux::PskType psk = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};

    auto ch1 = co_await dev1->CreateChannel(psk);
    auto res2 = std::make_shared<ResolverStaticEndpoint>(dev1->LocalEndpoint());
    auto ch2 = co_await dev2->CreateChannel(psk, res2);

    Cancel cancelObj;
    co_await Omni::Fiber::Yield();
    boost::asio::steady_timer waitTimer(io.get_executor());
    waitTimer.expires_after(std::chrono::milliseconds(20));
    co_await waitTimer.async_wait(Omni::Fiber::AsioUseFiber);
    co_await Omni::Fiber::Yield();

    Packet p1;
    std::string msg1 = "Init Migration!";
    p1._Offset = 2;
    std::copy(msg1.begin(), msg1.end(), p1._Data.begin() + p1._Offset);
    p1._Length = msg1.size();

    auto initialWriter = current.Spawn("writer", [&]() -> Omni::Fiber::Coroutine<void> {
      auto writeErr = co_await ch2->Write(p1, cancelObj);
      EXPECT_FALSE(writeErr);
      co_return;
    });

    Packet rxP;
    auto readErr = co_await ch1->Read(rxP, cancelObj);
    EXPECT_FALSE(readErr);
    co_await current.Join(initialWriter);

    auto dev2New =
        std::make_shared<UdpDynMux>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
    auto err3 = co_await dev2New->Start();
    EXPECT_FALSE(err3);

    auto ch2New = co_await dev2New->CreateChannel(psk, res2);

    co_await Omni::Fiber::Yield();
    boost::asio::steady_timer waitTimer2(io.get_executor());
    waitTimer2.expires_after(std::chrono::milliseconds(20));
    co_await waitTimer2.async_wait(Omni::Fiber::AsioUseFiber);
    co_await Omni::Fiber::Yield();

    Packet p2;
    std::string msg2 = "Migrated Packet!";
    p2._Offset = 2;
    std::copy(msg2.begin(), msg2.end(), p2._Data.begin() + p2._Offset);
    p2._Length = msg2.size();

    bool migrateReadPassed = false;
    auto readFiber = current.Spawn("mig_reader", [&]() -> Omni::Fiber::Coroutine<void> {
      Packet rxP2;
      auto readErr2 = co_await ch1->Read(rxP2, cancelObj);
      EXPECT_FALSE(readErr2);
      std::string rxMsg2(rxP2._Data.begin() + rxP2._Offset, rxP2._Data.begin() + rxP2._Offset + rxP2._Length);
      EXPECT_EQ(rxMsg2, msg2);
      migrateReadPassed = true;
      co_return;
    });

    auto migrateWriter = current.Spawn("mig_writer", [&]() -> Omni::Fiber::Coroutine<void> {
      auto writeErr = co_await ch2New->Write(p2, cancelObj);
      EXPECT_FALSE(writeErr);
      co_return;
    });

    co_await current.Join(readFiber);
    co_await current.Join(migrateWriter);
    EXPECT_TRUE(migrateReadPassed);

    co_await dev1->Stop();
    co_await dev2->Stop();
    co_await dev2New->Stop();

    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(UdpDynMuxTest, ReadCancellation) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto server =
      std::make_shared<UdpDynMux>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentFiber();

    auto err = co_await server->Start();
    EXPECT_FALSE(err);

    UdpDynMux::PskType psk = {5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
    auto channel = co_await server->CreateChannel(psk);
    EXPECT_NE(channel, nullptr);

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

TEST(UdpDynMuxTest, WriteCancellation) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto server =
      std::make_shared<UdpDynMux>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentFiber();

    auto err = co_await server->Start();
    EXPECT_FALSE(err);

    UdpDynMux::PskType psk = {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6};
    auto channel = co_await server->CreateChannel(psk);
    EXPECT_NE(channel, nullptr);

    Cancel cancelObj;
    cancelObj.Trigger();

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

TEST(UdpDynMuxTest, InvalidChannelAndRenegotiation) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto dev1 = std::make_shared<UdpDynMux>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  auto dev2 = std::make_shared<UdpDynMux>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentFiber();

    auto err1 = co_await dev1->Start();
    EXPECT_FALSE(err1);
    auto err2 = co_await dev2->Start();
    EXPECT_FALSE(err2);

    UdpDynMux::PskType psk = {8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8};

    auto ch1 = co_await dev1->CreateChannel(psk);
    EXPECT_NE(ch1, nullptr);

    auto resolver = std::make_shared<ResolverStaticEndpoint>(dev1->LocalEndpoint());
    auto ch2 = co_await dev2->CreateChannel(psk, resolver);
    EXPECT_NE(ch2, nullptr);

    Cancel cancelObj;
    co_await Omni::Fiber::Yield();
    boost::asio::steady_timer waitTimer(io.get_executor());
    waitTimer.expires_after(std::chrono::milliseconds(20));
    co_await waitTimer.async_wait(Omni::Fiber::AsioUseFiber);
    co_await Omni::Fiber::Yield();

    Packet p1;
    std::string msg1 = "Hello Symmetric Mux!";
    p1._Offset = 2;
    std::copy(msg1.begin(), msg1.end(), p1._Data.begin() + p1._Offset);
    p1._Length = msg1.size();
    auto writeFiber = current.Spawn("writer", [&]() -> Omni::Fiber::Coroutine<void> {
      auto writeErr = co_await ch2->Write(p1, cancelObj);
      EXPECT_FALSE(writeErr);
      co_return;
    });

    Packet rxP;
    auto readErr = co_await ch1->Read(rxP, cancelObj);
    EXPECT_FALSE(readErr);
    co_await current.Join(writeFiber);

    auto dev1Port = dev1->LocalEndpoint().port();
    co_await dev1->Stop();

    auto dev1New = std::make_shared<UdpDynMux>(io.get_executor(),
                                               udp::endpoint(boost::asio::ip::address_v6::loopback(), dev1Port));
    auto err3 = co_await dev1New->Start();
    EXPECT_FALSE(err3);

    auto ch1New = co_await dev1New->CreateChannel(psk);
    EXPECT_NE(ch1New, nullptr);

    Packet p_dummy;
    p_dummy._Offset = 2;
    p_dummy._Length = 0;
    co_await ch2->Write(p_dummy, cancelObj);

    co_await Omni::Fiber::Yield();
    boost::asio::steady_timer waitTimer2(io.get_executor());
    waitTimer2.expires_after(std::chrono::milliseconds(200));
    co_await waitTimer2.async_wait(Omni::Fiber::AsioUseFiber);
    co_await Omni::Fiber::Yield();

    Packet p2;
    std::string msg2 = "Renegotiated Data!";
    p2._Offset = 2;
    std::copy(msg2.begin(), msg2.end(), p2._Data.begin() + p2._Offset);
    p2._Length = msg2.size();
    Cancel cancelObj2;

    auto renegWriter = current.Spawn("reneg_writer", [&]() -> Omni::Fiber::Coroutine<void> {
      auto writeErr2 = co_await ch2->Write(p2, cancelObj2);
      EXPECT_FALSE(writeErr2);
      co_return;
    });

    auto renegReader = current.Spawn("reneg_reader", [&]() -> Omni::Fiber::Coroutine<void> {
      Packet rxP2;
      auto readErr2 = co_await ch1New->Read(rxP2, cancelObj2);
      EXPECT_FALSE(readErr2);
      std::string rxMsg(rxP2._Data.begin() + rxP2._Offset, rxP2._Data.begin() + rxP2._Offset + rxP2._Length);
      EXPECT_EQ(rxMsg, msg2);
    });

    co_await Omni::Fiber::Yield();
    boost::asio::steady_timer waitTimer3(io.get_executor());
    waitTimer3.expires_after(std::chrono::milliseconds(20));
    co_await waitTimer3.async_wait(Omni::Fiber::AsioUseFiber);
    cancelObj2.Trigger();
    co_await Omni::Fiber::Yield();

    co_await current.Join(renegReader);
    co_await current.Join(renegWriter);

    co_await dev1New->Stop();
    co_await dev2->Stop();

    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}
