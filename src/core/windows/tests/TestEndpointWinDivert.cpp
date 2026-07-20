#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "EndpointWinDivert.hpp"
#include "FakeWinDivert.hpp"
#include "Fiber.hpp"
#include "GetCurrentOmniFiber.hpp"
#include "Manager.hpp"
#include "OmniYield.hpp"
#include "Packet.hpp"

using namespace gh;

namespace {

class MockRouteCallback : public WinDivertRouteCallback {
public:
  Result RouteResultVal = Result::Normal;
  auto WinDivertRoute(Packet& packet, const WINDIVERT_ADDRESS& addr) -> Result override {
    std::string content(reinterpret_cast<const char*>(packet.Data().data()), packet._Length);
    if (content.find("Bypassed") != std::string::npos) {
      return Result::Bypass;
    }
    if (content.find("Discard") != std::string::npos) {
      return Result::Discard;
    }
    return RouteResultVal;
  }
};

void RunEventLoop(boost::asio::io_context& io) {
  io.restart();
  io.run();
}

} // namespace

TEST(WinDivertTest, StartStopOpenClose) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  MockRouteCallback callback;
  auto winDivert = std::make_shared<WinDivert>(io.get_executor(), "test_divert", 12, 34, callback);

  auto& controller = test::GetFakeWinDivertController();
  controller.Reset();

  bool opened = false;
  bool closed = false;
  HANDLE openedHandle = INVALID_HANDLE_VALUE;

  controller.SetOpenCallback([&](HANDLE handle, const char* filter, WINDIVERT_LAYER layer) {
    opened = true;
    openedHandle = handle;
    EXPECT_EQ(layer, WINDIVERT_LAYER_NETWORK);
  });

  controller.SetCloseCallback([&](HANDLE handle) {
    closed = true;
    EXPECT_EQ(handle, openedHandle);
  });

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto err = co_await winDivert->Start();
    EXPECT_FALSE(err);
    EXPECT_TRUE(opened);

    err = co_await winDivert->Stop();
    EXPECT_FALSE(err);
    EXPECT_TRUE(closed);
    co_return;
  });

  RunEventLoop(io);
}

TEST(WinDivertTest, WritePacket) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  MockRouteCallback callback;
  auto winDivert = std::make_shared<WinDivert>(io.get_executor(), "test_divert", 12, 34, callback);

  auto& controller = test::GetFakeWinDivertController();
  controller.Reset();

  std::vector<uint8_t> sentData;
  WINDIVERT_ADDRESS sentAddr{};
  bool sendCalled = false;

  controller.SetSendCallback([&](HANDLE handle, const std::vector<uint8_t>& packet, const WINDIVERT_ADDRESS& addr) {
    sendCalled = true;
    sentData = packet;
    sentAddr = addr;
  });

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto err = co_await winDivert->Start();
    EXPECT_FALSE(err);
    if (err) {
      co_return;
    }

    Packet packet;
    std::string testMsg = "Hello WinDivert!";
    std::copy(testMsg.begin(), testMsg.end(), packet._Data.begin() + packet._Offset);
    packet._Length = testMsg.size();

    Cancel cancel;
    err = co_await winDivert->Write(packet, cancel);
    EXPECT_FALSE(err);
    EXPECT_TRUE(sendCalled);

    std::string sentMsg(sentData.begin(), sentData.end());
    EXPECT_EQ(sentMsg, testMsg);
    EXPECT_EQ(sentAddr.Network.IfIdx, 12);
    EXPECT_EQ(sentAddr.Network.SubIfIdx, 34);

    co_await winDivert->Stop();
    co_return;
  });

  RunEventLoop(io);
}

TEST(WinDivertTest, ReadNormalPacket) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  MockRouteCallback callback;
  callback.RouteResultVal = WinDivertRouteCallback::Result::Normal;

  auto winDivert = std::make_shared<WinDivert>(io.get_executor(), "test_divert", 12, 34, callback);

  auto& controller = test::GetFakeWinDivertController();
  controller.Reset();

  HANDLE targetHandle = INVALID_HANDLE_VALUE;
  controller.SetOpenCallback([&](HANDLE handle, const char* filter, WINDIVERT_LAYER layer) { targetHandle = handle; });

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto err = co_await winDivert->Start();
    EXPECT_FALSE(err);
    if (err) {
      co_return;
    }
    EXPECT_NE(targetHandle, INVALID_HANDLE_VALUE);
    if (targetHandle == INVALID_HANDLE_VALUE) {
      co_return;
    }

    // Push a packet from the driver end
    std::string mockData = "DriverPacket";
    std::vector<uint8_t> packetData(mockData.begin(), mockData.end());
    WINDIVERT_ADDRESS addr{};
    addr.Network.IfIdx = 99;

    controller.PushRecvPacket(targetHandle, packetData, addr);

    Packet readPacket;
    Cancel cancel;
    err = co_await winDivert->Read(readPacket, cancel);
    EXPECT_FALSE(err);

    std::string readMsg(readPacket.Data().begin(), readPacket.Data().end());
    EXPECT_EQ(readMsg, mockData);

    co_await winDivert->Stop();
    co_return;
  });

  RunEventLoop(io);
}

TEST(WinDivertTest, ReadBypassAndDiscard) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  MockRouteCallback callback;
  auto winDivert = std::make_shared<WinDivert>(io.get_executor(), "test_divert", 12, 34, callback);

  auto& controller = test::GetFakeWinDivertController();
  controller.Reset();

  HANDLE targetHandle = INVALID_HANDLE_VALUE;
  controller.SetOpenCallback([&](HANDLE handle, const char* filter, WINDIVERT_LAYER layer) { targetHandle = handle; });

  bool bypassSent = false;
  std::vector<uint8_t> bypassData;

  controller.SetSendCallback([&](HANDLE handle, const std::vector<uint8_t>& packet, const WINDIVERT_ADDRESS& addr) {
    bypassSent = true;
    bypassData = packet;
  });

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto err = co_await winDivert->Start();
    EXPECT_FALSE(err);
    if (err) {
      co_return;
    }

    // 1. Test Bypass:
    callback.RouteResultVal = WinDivertRouteCallback::Result::Bypass;
    std::string bypassMsg = "BypassedTraffic";
    WINDIVERT_ADDRESS addr1{};
    addr1.Network.IfIdx = 100;

    controller.PushRecvPacket(targetHandle, std::vector<uint8_t>(bypassMsg.begin(), bypassMsg.end()), addr1);

    // 2. Test Discard:
    callback.RouteResultVal = WinDivertRouteCallback::Result::Discard;
    std::string discardMsg = "DiscardTraffic";
    WINDIVERT_ADDRESS addr2{};
    addr2.Network.IfIdx = 101;

    controller.PushRecvPacket(targetHandle, std::vector<uint8_t>(discardMsg.begin(), discardMsg.end()), addr2);

    // 3. Test Normal (to allow the Read call to complete and test loop behavior):
    callback.RouteResultVal = WinDivertRouteCallback::Result::Normal;
    std::string normalMsg = "NormalTraffic";
    WINDIVERT_ADDRESS addr3{};
    addr3.Network.IfIdx = 102;

    controller.PushRecvPacket(targetHandle, std::vector<uint8_t>(normalMsg.begin(), normalMsg.end()), addr3);

    Packet readPacket;
    Cancel cancel;
    err = co_await winDivert->Read(readPacket, cancel);
    EXPECT_FALSE(err);

    // Should read "NormalTraffic" since the other two were bypassed/discarded internally
    std::string readMsg(readPacket.Data().begin(), readPacket.Data().end());
    EXPECT_EQ(readMsg, normalMsg);

    // Check bypass send callback was indeed called
    EXPECT_TRUE(bypassSent);
    std::string sentBypass(bypassData.begin(), bypassData.end());
    EXPECT_EQ(sentBypass, bypassMsg);

    co_await winDivert->Stop();
    co_return;
  });

  RunEventLoop(io);
}

TEST(WinDivertTest, ReadInjectedPacketPrioritized) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  MockRouteCallback callback;
  auto winDivert = std::make_shared<WinDivert>(io.get_executor(), "test_divert", 12, 34, callback);

  auto& controller = test::GetFakeWinDivertController();
  controller.Reset();

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto err = co_await winDivert->Start();
    EXPECT_FALSE(err);
    if (err) {
      co_return;
    }

    // Inject a packet
    Packet injectPacket;
    std::string injectMsg = "InjectedPacket";
    std::copy(injectMsg.begin(), injectMsg.end(), injectPacket._Data.begin() + injectPacket._Offset);
    injectPacket._Length = injectMsg.size();

    co_await winDivert->Inject(std::move(injectPacket), WINDIVERT_ADDRESS{}, WinDivertRouteCallback::Result::Normal);

    Packet readPacket;
    Cancel cancel;
    err = co_await winDivert->Read(readPacket, cancel);
    EXPECT_FALSE(err);

    std::string readMsg(readPacket.Data().begin(), readPacket.Data().end());
    EXPECT_EQ(readMsg, injectMsg);

    co_await winDivert->Stop();
    co_return;
  });

  RunEventLoop(io);
}

TEST(WinDivertTest, ReadCancelledByStop) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  MockRouteCallback callback;
  auto winDivert = std::make_shared<WinDivert>(io.get_executor(), "test_divert", 12, 34, callback);

  auto& controller = test::GetFakeWinDivertController();
  controller.Reset();

  bool readCompleted = false;
  ErrorCode readErr{};

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto err = co_await winDivert->Start();
    EXPECT_FALSE(err);
    if (err) {
      co_return;
    }

    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentOmniFiber();
    auto reader = current.Spawn("reader", [&]() -> Omni::Fiber::Coroutine<void> {
      Packet readPacket;
      Cancel cancel;
      readErr = co_await winDivert->Read(readPacket, cancel);
      readCompleted = true;
      co_return;
    });

    co_await Omni::Fiber::OmniYield();

    auto stopErr = co_await winDivert->Stop();
    EXPECT_FALSE(stopErr);

    co_await current.Join(reader);

    EXPECT_TRUE(readCompleted);
    EXPECT_EQ(readErr.value(), boost::asio::error::operation_aborted);

    co_return;
  });

  RunEventLoop(io);
}

TEST(WinDivertTest, ReadCancelledByCancelObject) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  MockRouteCallback callback;
  auto winDivert = std::make_shared<WinDivert>(io.get_executor(), "test_divert", 12, 34, callback);

  auto& controller = test::GetFakeWinDivertController();
  controller.Reset();

  bool readCompleted = false;
  ErrorCode readErr{};

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto err = co_await winDivert->Start();
    EXPECT_FALSE(err);
    if (err) {
      co_return;
    }

    Omni::Fiber::Fiber& current = co_await Omni::Fiber::GetCurrentOmniFiber();
    Cancel cancel;
    auto reader = current.Spawn("reader", [&]() -> Omni::Fiber::Coroutine<void> {
      Packet readPacket;
      readErr = co_await winDivert->Read(readPacket, cancel);
      readCompleted = true;
      co_return;
    });

    co_await Omni::Fiber::OmniYield();

    // Trigger the cancel object instead of calling Stop()!
    cancel.Trigger();

    // Wait a bit to let the scheduler run
    boost::asio::steady_timer timer(io);
    timer.expires_after(std::chrono::milliseconds(50));
    co_await timer.async_wait(Omni::Fiber::AsioUseFiber);

    EXPECT_TRUE(readCompleted);
    EXPECT_EQ(readErr.value(), boost::asio::error::operation_aborted);

    co_await winDivert->Stop();
    co_await current.Join(reader);
    co_return;
  });

  RunEventLoop(io);
}
