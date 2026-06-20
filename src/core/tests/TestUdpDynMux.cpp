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

TEST(UdpDynMuxTest, IncompatibleVersionNegotiation) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto dev = std::make_shared<UdpDynMux>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentFiber();

    auto err = co_await dev->Start();
    EXPECT_FALSE(err);

    UdpDynMux::PskType psk = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    auto ch = co_await dev->CreateChannel(psk);
    EXPECT_NE(ch, nullptr);

    // Create a raw UDP socket to act as the incompatible client
    udp::socket rawSocket(io);
    rawSocket.open(udp::v6());
    rawSocket.bind(udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

    // Prepare incompatible INITIATE packet (Major = 99, Minor = 0)
    std::array<uint8_t, 27> badInit{};
    badInit[0] = 0;
    badInit[1] = 0;
    badInit[2] = 0x01; // MsgType::kInitiate
    std::copy(psk.begin(), psk.end(), badInit.begin() + 3);
    // My Rx Channel ID = 1234
    badInit[19] = 0x04;
    badInit[20] = 0xD2;
    // Peer Rx Channel ID = server's localRxId
    uint16_t localRxId = ch->GetLocalRxId();
    badInit[21] = (localRxId >> 8) & 0xff;
    badInit[22] = localRxId & 0xff;
    // Version: Major 99, Minor 0, Patch 0
    badInit[23] = 99;
    badInit[24] = 0;
    badInit[25] = 0;
    badInit[26] = 0;

    // Send it to the server
    co_await rawSocket.async_send_to(boost::asio::buffer(badInit), dev->LocalEndpoint(), Omni::Fiber::AsioUseFiber);

    // Receive the expected response
    std::array<uint8_t, 100> rxBuf{};
    udp::endpoint sender;
    auto [recvErr, bytes] = co_await rawSocket.async_receive_from(boost::asio::buffer(rxBuf), sender, Omni::Fiber::AsioUseFiber);
    EXPECT_FALSE(recvErr);
    EXPECT_EQ(bytes, 19); // INITIATE_FAIL size is 19 bytes
    EXPECT_EQ(rxBuf[0], 0);
    EXPECT_EQ(rxBuf[1], 0);
    EXPECT_EQ(rxBuf[2], 0x02); // MsgType::kInitiateFail

    // The channel should stop because of incompatible version.
    // Wait for the channel to transition to kFinished.
    boost::asio::steady_timer waitTimer2(io.get_executor());
    for (int i = 0; i < 50; ++i) {
      if (ch->GetState() == ServiceBase::State::kFinished || ch->GetState() == ServiceBase::State::kError) {
        break;
      }
      waitTimer2.expires_after(std::chrono::milliseconds(10));
      co_await waitTimer2.async_wait(Omni::Fiber::AsioUseFiber);
    }
    EXPECT_EQ(ch->GetState(), ServiceBase::State::kFinished);

    co_await dev->Stop();
    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(UdpDynMuxTest, KeepaliveRttHandshake) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto dev = std::make_shared<UdpDynMux>(io.get_executor(), udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentFiber();

    auto err = co_await dev->Start();
    EXPECT_FALSE(err);

    UdpDynMux::PskType psk = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    auto ch = co_await dev->CreateChannel(psk);
    EXPECT_NE(ch, nullptr);

    udp::socket rawSocket(io);
    rawSocket.open(udp::v6());
    rawSocket.bind(udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

    uint16_t clientRxId = 5555;
    uint16_t serverRxId = ch->GetLocalRxId();

    // Phase 1: Establish the channel (Negotiation)
    std::array<uint8_t, 27> initMsg{};
    initMsg[0] = 0; initMsg[1] = 0; initMsg[2] = 0x01; // MsgType::kInitiate
    std::copy(psk.begin(), psk.end(), initMsg.begin() + 3);
    initMsg[19] = (clientRxId >> 8) & 0xff;
    initMsg[20] = clientRxId & 0xff;
    initMsg[21] = 0;
    initMsg[22] = 0;
    initMsg[23] = UdpDynMuxProto::kMajorVersion;
    initMsg[24] = UdpDynMuxProto::kMinorVersion;
    initMsg[25] = (UdpDynMuxProto::kPatchVersion >> 8) & 0xff;
    initMsg[26] = UdpDynMuxProto::kPatchVersion & 0xff;

    co_await rawSocket.async_send_to(boost::asio::buffer(initMsg), dev->LocalEndpoint(), Omni::Fiber::AsioUseFiber);

    // Receive server's initiate
    std::array<uint8_t, 100> rxBuf{};
    udp::endpoint sender;
    auto [recvErr, bytes] = co_await rawSocket.async_receive_from(boost::asio::buffer(rxBuf), sender, Omni::Fiber::AsioUseFiber);
    EXPECT_FALSE(recvErr);
    EXPECT_EQ(bytes, 27);
    EXPECT_EQ(rxBuf[2], 0x01);

    // Send final initiate with serverRxId populated
    initMsg[21] = (serverRxId >> 8) & 0xff;
    initMsg[22] = serverRxId & 0xff;
    co_await rawSocket.async_send_to(boost::asio::buffer(initMsg), dev->LocalEndpoint(), Omni::Fiber::AsioUseFiber);

    // Yield to let the server transition to kRunning
    co_await Omni::Fiber::Yield();
    boost::asio::steady_timer waitTimer(io.get_executor());
    waitTimer.expires_after(std::chrono::milliseconds(20));
    co_await waitTimer.async_wait(Omni::Fiber::AsioUseFiber);

    // Phase 2: Send Keepalive Ping (Packet 1)
    std::array<uint8_t, 20> pingMsg{};
    pingMsg[0] = 0; pingMsg[1] = 0; pingMsg[2] = 0x03; // MsgType::kKeepalive
    std::copy(psk.begin(), psk.end(), pingMsg.begin() + 3);
    pingMsg[19] = 0x01; // Ping=1, Pong=0

    co_await rawSocket.async_send_to(boost::asio::buffer(pingMsg), dev->LocalEndpoint(), Omni::Fiber::AsioUseFiber);

    // Receive Packet 2 from server (should be Ping=1, Pong=1)
    auto [recvErr2, bytes2] = co_await rawSocket.async_receive_from(boost::asio::buffer(rxBuf), sender, Omni::Fiber::AsioUseFiber);
    EXPECT_FALSE(recvErr2);
    EXPECT_EQ(bytes2, 20);
    EXPECT_EQ(rxBuf[2], 0x03);
    EXPECT_EQ(rxBuf[19], 0x03); // Ping=1, Pong=1

    // Send Packet 3 to server (Ping=0, Pong=1)
    std::array<uint8_t, 20> pongMsg{};
    pongMsg[0] = 0; pongMsg[1] = 0; pongMsg[2] = 0x03;
    std::copy(psk.begin(), psk.end(), pongMsg.begin() + 3);
    pongMsg[19] = 0x02; // Ping=0, Pong=1

    co_await rawSocket.async_send_to(boost::asio::buffer(pongMsg), dev->LocalEndpoint(), Omni::Fiber::AsioUseFiber);

    // Yield to let the server process Packet 3 and measure RTT
    co_await Omni::Fiber::Yield();

    // Verify server does not send any further keepalive packets
    boost::asio::steady_timer checkTimer(io.get_executor());
    checkTimer.expires_after(std::chrono::milliseconds(50));
    co_await checkTimer.async_wait(Omni::Fiber::AsioUseFiber);
    EXPECT_EQ(rawSocket.available(), 0);

    co_await dev->Stop();
    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(UdpDynMuxTest, InvalidAddressAndRenegotiation) {
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
    // Wait for negotiation to complete
    co_await Omni::Fiber::Yield();
    boost::asio::steady_timer waitTimer(io.get_executor());
    waitTimer.expires_after(std::chrono::milliseconds(20));
    co_await waitTimer.async_wait(Omni::Fiber::AsioUseFiber);
    co_await Omni::Fiber::Yield();

    // Verify bidirectional communication works first
    Packet p1;
    std::string msg1 = "Hello Mux!";
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

    // --- Part 1: Verify Dev 1 sends INVALID_ADDRESS when receiving data from unrecognized address ---
    udp::socket rawSocket(io);
    rawSocket.open(udp::v6());
    rawSocket.bind(udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

    // Send a data packet to Dev 1 with channel ID ch1->GetLocalRxId() from rawSocket
    std::array<uint8_t, 10> dataPkt{};
    dataPkt[0] = (ch1->GetLocalRxId() >> 8) & 0xff;
    dataPkt[1] = ch1->GetLocalRxId() & 0xff;
    dataPkt[2] = 'A';
    dataPkt[3] = 'B';
    
    co_await rawSocket.async_send_to(boost::asio::buffer(dataPkt), dev1->LocalEndpoint(), Omni::Fiber::AsioUseFiber);

    // Expect INVALID_ADDRESS back on rawSocket
    std::array<uint8_t, 100> rxBuf{};
    udp::endpoint sender;
    auto [recvErr, bytes] = co_await rawSocket.async_receive_from(boost::asio::buffer(rxBuf), sender, Omni::Fiber::AsioUseFiber);
    EXPECT_FALSE(recvErr);
    EXPECT_EQ(bytes, 5); // INVALID_ADDRESS is 5 bytes
    EXPECT_EQ(rxBuf[0], 0);
    EXPECT_EQ(rxBuf[1], 0);
    EXPECT_EQ(rxBuf[2], 0x0B); // MsgType::kInvalidAddress
    uint16_t reportedChannelId = (rxBuf[3] << 8) | rxBuf[4];
    EXPECT_EQ(reportedChannelId, ch1->GetLocalRxId());

    // --- Part 2: Verify Dev 2 transitions to kNegotiating without clearing _Peer when receiving INVALID_ADDRESS ---
    std::array<uint8_t, 5> invalidAddrPkt{};
    invalidAddrPkt[0] = 0;
    invalidAddrPkt[1] = 0;
    invalidAddrPkt[2] = 0x0B; // MsgType::kInvalidAddress
    // Channel ID matches ch2->GetRemoteRxId() (which is ch1->GetLocalRxId())
    invalidAddrPkt[3] = (ch2->GetRemoteRxId() >> 8) & 0xff;
    invalidAddrPkt[4] = ch2->GetRemoteRxId() & 0xff;

    // Send INVALID_ADDRESS to Dev 2 pretending to be Dev 1
    Packet dev1ErrPkt;
    dev1ErrPkt._Offset = 0;
    std::copy(invalidAddrPkt.begin(), invalidAddrPkt.end(), dev1ErrPkt._Data.begin());
    dev1ErrPkt._Length = invalidAddrPkt.size();

    Cancel c;
    auto writeErr = co_await dev1->WriteTo(dev2->LocalEndpoint(), dev1ErrPkt, c);
    EXPECT_FALSE(writeErr);

    // Wait for Dev 2 to receive the packet and transition
    co_await Omni::Fiber::Yield();
    boost::asio::steady_timer waitTimer2(io.get_executor());
    waitTimer2.expires_after(std::chrono::milliseconds(50));
    co_await waitTimer2.async_wait(Omni::Fiber::AsioUseFiber);

    // Verify Dev 2 is in negotiating state and sends INITIATE to Dev 1
    Packet p2;
    std::string msg2 = "Renegotiated Mux!";
    p2._Offset = 2;
    std::copy(msg2.begin(), msg2.end(), p2._Data.begin() + p2._Offset);
    p2._Length = msg2.size();
    
    auto writeFiber2 = current.Spawn("writer2", [&]() -> Omni::Fiber::Coroutine<void> {
      auto writeErr2 = co_await ch2->Write(p2, cancelObj);
      EXPECT_FALSE(writeErr2);
      co_return;
    });

    Packet rxP2;
    auto readErr2 = co_await ch1->Read(rxP2, cancelObj);
    EXPECT_FALSE(readErr2);
    co_await current.Join(writeFiber2);

    co_await dev1->Stop();
    co_await dev2->Stop();

    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

