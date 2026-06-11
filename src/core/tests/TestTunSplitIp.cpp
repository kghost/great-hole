#include <memory>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "EndpointTunSplitIp.hpp"
#include "ErrorCode.hpp"
#include "Fiber.hpp"
#include "GetCurrentFiber.hpp"
#include "Manager.hpp"
#include "Packet.hpp"
#include "Utils.hpp"

using namespace gh;

namespace {

void RunEventLoop(boost::asio::io_context& io) {
  io.restart();
  io.run();
}

} // namespace

TEST(TunSplitIpTest, DispatchAndVerifyIP) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  int fds[2];
  ASSERT_GE(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds), 0);

  int testFd = fds[0];
  int externalFd = fds[1];

  auto tunSplit = std::make_shared<EndpointTunSplitIp>(io, "test", testFd);
  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentFiber();

    auto err = co_await tunSplit->Start();
    EXPECT_FALSE(err);
    if (err) {
      ::close(externalFd);
      co_return;
    }

    auto ip1 = MapToV6(boost::asio::ip::make_address("10.0.0.1"));
    auto ip2 = MapToV6(boost::asio::ip::make_address("10.0.0.2"));

    auto channel1 = co_await tunSplit->CreateChannel({ip1});
    auto channel2 = co_await tunSplit->CreateChannel({ip2});

    EXPECT_NE(channel1, nullptr);
    EXPECT_NE(channel2, nullptr);
    if (channel1 == nullptr || channel2 == nullptr) {
      ::close(externalFd);
      co_return;
    }

    // Test Case 1: Send packet from external side with dest = ip1 (10.0.0.1)
    // It should be received by channel1
    std::vector<uint8_t> rawPacket1(20);
    rawPacket1[0] = 0x45; // IPv4
    // Src = 192.168.1.1
    rawPacket1[12] = 192;
    rawPacket1[13] = 168;
    rawPacket1[14] = 1;
    rawPacket1[15] = 1;
    // Dest = 10.0.0.1
    rawPacket1[16] = 10;
    rawPacket1[17] = 0;
    rawPacket1[18] = 0;
    rawPacket1[19] = 1;

    ssize_t written = ::write(externalFd, rawPacket1.data(), rawPacket1.size());
    EXPECT_EQ(written, static_cast<ssize_t>(rawPacket1.size()));

    Cancel cancelObj;
    Packet p1;
    auto readErr = co_await channel1->Read(p1, cancelObj);
    EXPECT_FALSE(readErr);
    EXPECT_EQ(p1.DataSize(), rawPacket1.size());
    EXPECT_EQ(p1.Data()[16], 10);
    EXPECT_EQ(p1.Data()[19], 1);

    // Test Case 2: Send packet from external side with dest = unknown IP (10.0.0.3)
    // It should be dropped, not received by channel1 or channel2
    std::vector<uint8_t> rawPacketUnknown(20);
    rawPacketUnknown[0] = 0x45;
    // Src = 192.168.1.1
    rawPacketUnknown[12] = 192;
    rawPacketUnknown[13] = 168;
    rawPacketUnknown[14] = 1;
    rawPacketUnknown[15] = 1;
    // Dest = 10.0.0.3
    rawPacketUnknown[16] = 10;
    rawPacketUnknown[17] = 0;
    rawPacketUnknown[18] = 0;
    rawPacketUnknown[19] = 3;

    written = ::write(externalFd, rawPacketUnknown.data(), rawPacketUnknown.size());
    EXPECT_EQ(written, static_cast<ssize_t>(rawPacketUnknown.size()));

    // Test Case 3: Send valid IPv6 packet from external side with dest = ipV6
    auto ipV6 = MapToV6(boost::asio::ip::make_address("fd00::1"));
    auto channelV6 = co_await tunSplit->CreateChannel({ipV6});
    EXPECT_NE(channelV6, nullptr);
    if (channelV6 == nullptr) {
      co_await tunSplit->RemoveChannel(channel1);
      co_await tunSplit->RemoveChannel(channel2);
      co_await tunSplit->Stop();
      ::close(externalFd);
      co_return;
    }

    std::vector<uint8_t> rawPacketV6(40);
    rawPacketV6[0] = 0x60; // IPv6 version 6 (upper nibble)
    // Src = fd00::2 (offset 8) -> {0xfd, 0x00, ..., 0x02}
    rawPacketV6[8] = 0xfd;
    rawPacketV6[23] = 2;
    // Dest = fd00::1 (offset 24) -> {0xfd, 0x00, ..., 0x01}
    rawPacketV6[24] = 0xfd;
    rawPacketV6[39] = 1;

    written = ::write(externalFd, rawPacketV6.data(), rawPacketV6.size());
    EXPECT_EQ(written, static_cast<ssize_t>(rawPacketV6.size()));

    Packet pV6;
    readErr = co_await channelV6->Read(pV6, cancelObj);
    EXPECT_FALSE(readErr);
    EXPECT_EQ(pV6.DataSize(), rawPacketV6.size());
    EXPECT_EQ(pV6.Data()[24], 0xfd);
    EXPECT_EQ(pV6.Data()[39], 1);

    // Test Case 4: Channel write packet validation (matching source IP)
    // channel1 IP is 10.0.0.1.
    // 4a: writing a packet with src = 10.0.0.1 should succeed
    Packet sendPacketValid;
    sendPacketValid._Length = 20;
    sendPacketValid.Data()[0] = 0x45;
    // Src = 10.0.0.1
    sendPacketValid.Data()[12] = 10;
    sendPacketValid.Data()[13] = 0;
    sendPacketValid.Data()[14] = 0;
    sendPacketValid.Data()[15] = 1;
    // Dest = 192.168.1.1
    sendPacketValid.Data()[16] = 192;
    sendPacketValid.Data()[17] = 168;
    sendPacketValid.Data()[18] = 1;
    sendPacketValid.Data()[19] = 1;

    auto writeErr = co_await channel1->Write(sendPacketValid, cancelObj);
    EXPECT_FALSE(writeErr);

    // Verify it was written to externalFd
    std::vector<uint8_t> readBuf(20);
    ssize_t readBytes = ::read(externalFd, readBuf.data(), readBuf.size());
    EXPECT_EQ(readBytes, 20);
    EXPECT_EQ(readBuf[12], 10);
    EXPECT_EQ(readBuf[15], 1);

    // 4b: writing a packet with src = 10.0.0.2 (mismatched) from channel1 should fail (be dropped)
    Packet sendPacketInvalid;
    sendPacketInvalid._Length = 20;
    sendPacketInvalid.Data()[0] = 0x45;
    // Src = 10.0.0.2
    sendPacketInvalid.Data()[12] = 10;
    sendPacketInvalid.Data()[13] = 0;
    sendPacketInvalid.Data()[14] = 0;
    sendPacketInvalid.Data()[15] = 2;
    // Dest = 192.168.1.1
    sendPacketInvalid.Data()[16] = 192;
    sendPacketInvalid.Data()[17] = 168;
    sendPacketInvalid.Data()[18] = 1;
    sendPacketInvalid.Data()[19] = 1;

    writeErr = co_await channel1->Write(sendPacketInvalid, cancelObj);
    EXPECT_EQ(writeErr, ErrorCode(AppMinorErrorCategory::kSourceIpMismatch, kAppMinorError));

    // Cleanup channels
    co_await tunSplit->RemoveChannel(channel1);
    co_await tunSplit->RemoveChannel(channel2);
    co_await tunSplit->RemoveChannel(channelV6);

    auto stopErr = co_await tunSplit->Stop();
    EXPECT_FALSE(stopErr);

    ::close(externalFd);
    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(TunSplitIpTest, MultipleIPsPerChannel) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  int fds[2];
  ASSERT_GE(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds), 0);

  int testFd = fds[0];
  int externalFd = fds[1];

  auto tunSplit = std::make_shared<EndpointTunSplitIp>(io, "test", testFd);
  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentFiber();

    auto err = co_await tunSplit->Start();
    EXPECT_FALSE(err);
    if (err) {
      ::close(externalFd);
      co_return;
    }

    auto ip1 = MapToV6(boost::asio::ip::make_address("10.0.0.1"));
    auto ip2 = MapToV6(boost::asio::ip::make_address("10.0.0.10"));

    auto channel = co_await tunSplit->CreateChannel({ip1, ip2});
    EXPECT_NE(channel, nullptr);
    if (channel == nullptr) {
      co_await tunSplit->Stop();
      ::close(externalFd);
      co_return;
    }

    Cancel cancelObj;

    // Test Case 1: Send packet to ip1, should receive
    std::vector<uint8_t> rawPacket1(20);
    rawPacket1[0] = 0x45;
    rawPacket1[12] = 192;
    rawPacket1[13] = 168;
    rawPacket1[14] = 1;
    rawPacket1[15] = 1;
    rawPacket1[16] = 10;
    rawPacket1[17] = 0;
    rawPacket1[18] = 0;
    rawPacket1[19] = 1;
    ssize_t written = ::write(externalFd, rawPacket1.data(), rawPacket1.size());
    EXPECT_EQ(written, static_cast<ssize_t>(rawPacket1.size()));

    Packet p1;
    auto readErr = co_await channel->Read(p1, cancelObj);
    EXPECT_FALSE(readErr);
    EXPECT_EQ(p1.DataSize(), 20);
    EXPECT_EQ(p1.Data()[19], 1);

    // Test Case 2: Send packet to ip2, should receive on the same channel
    std::vector<uint8_t> rawPacket2(20);
    rawPacket2[0] = 0x45;
    rawPacket2[12] = 192;
    rawPacket2[13] = 168;
    rawPacket2[14] = 1;
    rawPacket2[15] = 1;
    rawPacket2[16] = 10;
    rawPacket2[17] = 0;
    rawPacket2[18] = 0;
    rawPacket2[19] = 10;
    written = ::write(externalFd, rawPacket2.data(), rawPacket2.size());
    EXPECT_EQ(written, static_cast<ssize_t>(rawPacket2.size()));

    Packet p2;
    readErr = co_await channel->Read(p2, cancelObj);
    EXPECT_FALSE(readErr);
    EXPECT_EQ(p2.DataSize(), 20);
    EXPECT_EQ(p2.Data()[19], 10);

    // Test Case 3: Write packet from channel with source IP = ip1, should succeed
    Packet sendPacket1;
    sendPacket1._Length = 20;
    sendPacket1.Data()[0] = 0x45;
    sendPacket1.Data()[12] = 10;
    sendPacket1.Data()[13] = 0;
    sendPacket1.Data()[14] = 0;
    sendPacket1.Data()[15] = 1;
    sendPacket1.Data()[16] = 192;
    sendPacket1.Data()[17] = 168;
    sendPacket1.Data()[18] = 1;
    sendPacket1.Data()[19] = 1;

    auto writeErr = co_await channel->Write(sendPacket1, cancelObj);
    EXPECT_FALSE(writeErr);

    std::vector<uint8_t> readBuf(20);
    ssize_t readBytes = ::read(externalFd, readBuf.data(), readBuf.size());
    EXPECT_EQ(readBytes, 20);
    EXPECT_EQ(readBuf[15], 1);

    // Test Case 4: Write packet from channel with source IP = ip2, should succeed
    Packet sendPacket2;
    sendPacket2._Length = 20;
    sendPacket2.Data()[0] = 0x45;
    sendPacket2.Data()[12] = 10;
    sendPacket2.Data()[13] = 0;
    sendPacket2.Data()[14] = 0;
    sendPacket2.Data()[15] = 10;
    sendPacket2.Data()[16] = 192;
    sendPacket2.Data()[17] = 168;
    sendPacket2.Data()[18] = 1;
    sendPacket2.Data()[19] = 1;

    writeErr = co_await channel->Write(sendPacket2, cancelObj);
    EXPECT_FALSE(writeErr);

    readBytes = ::read(externalFd, readBuf.data(), readBuf.size());
    EXPECT_EQ(readBytes, 20);
    EXPECT_EQ(readBuf[15], 10);

    // Cleanup channels
    co_await tunSplit->RemoveChannel(channel);
    auto stopErr = co_await tunSplit->Stop();
    EXPECT_FALSE(stopErr);

    ::close(externalFd);
    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}
