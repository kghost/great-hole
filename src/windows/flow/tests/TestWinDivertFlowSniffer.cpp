#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/trivial.hpp>
#include <cstring>
#include <memory>
#include <vector>

#include <boost/asio.hpp>
#include <gtest/gtest.h>
#include <windivert.h>
#include <windows.h>
#include <winsock2.h>

#include "Asio.hpp"
#include "Coroutine.hpp"
#include "FakeWinDivert.hpp"
#include "Fiber.hpp"
#include "Manager.hpp"
#include "WinDivertFlowSniffer.hpp"

using namespace gh;

namespace {

class MockFlowSnifferCallback : public WinDivertFlowSnifferCallback {
public:
  struct EstablishedEvent {
    ConnectionTracker::ConnectionKey Conn;
    uint32_t Pid;
  };

  struct DeletedEvent {
    ConnectionTracker::ConnectionKey Conn;
  };

  std::vector<EstablishedEvent> EstablishedEvents;
  std::vector<DeletedEvent> DeletedEvents;

  auto OnFlowEstablished(const ConnectionTracker::ConnectionKey& conn, uint32_t pid)
      -> Omni::Fiber::Coroutine<void> override {
    EstablishedEvents.push_back({conn, pid});
    co_return;
  }

  auto OnFlowDeleted(const ConnectionTracker::ConnectionKey& conn) -> Omni::Fiber::Coroutine<void> override {
    DeletedEvents.push_back({conn});
    co_return;
  }

  void Clear() {
    EstablishedEvents.clear();
    DeletedEvents.clear();
  }
};

void RunEventLoop(boost::asio::io_context& io) {
  io.restart();
  io.run();
}

void SetFlowAddress(UINT32* outAddr, const boost::asio::ip::address& addr) {
  boost::asio::ip::address_v6 v6Addr;
  if (addr.is_v4()) {
    v6Addr = boost::asio::ip::make_address_v6(boost::asio::ip::v4_mapped, addr.to_v4());
  } else {
    v6Addr = addr.to_v6();
  }
  auto bytes = v6Addr.to_bytes();
  std::memcpy(outAddr, bytes.data(), 16);
  for (int i = 0; i < 4; ++i) {
    outAddr[i] = ntohl(outAddr[i]);
  }
}

void PushFlowEvent(test::FakeWinDivertController& controller, HANDLE handle, WINDIVERT_EVENT event, uint32_t protocol,
                   const boost::asio::ip::address& localAddr, uint16_t localPort,
                   const boost::asio::ip::address& remoteAddr, uint16_t remotePort, uint32_t pid, bool isIpv6) {
  WINDIVERT_ADDRESS addr{};
  addr.Layer = WINDIVERT_LAYER_FLOW;
  addr.Event = event;
  addr.IPv6 = isIpv6 ? 1 : 0;

  auto& flow = addr.Flow;
  flow.ProcessId = pid;
  flow.Protocol = protocol;
  flow.LocalPort = localPort;
  flow.RemotePort = remotePort;

  SetFlowAddress(flow.LocalAddr, localAddr);
  SetFlowAddress(flow.RemoteAddr, remoteAddr);

  controller.PushRecvPacket(handle, {}, addr);
}

} // namespace

TEST(WinDivertFlowSnifferTest, StartStop) {
  boost::log::core::get()->set_filter([](const boost::log::attribute_value_set& attrs) -> bool {
    auto value = boost::log::extract<boost::log::trivial::severity_level>("Severity", attrs);
    return value && value.get() >= boost::log::trivial::trace;
  });

  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  MockFlowSnifferCallback callback;
  auto sniffer = std::make_shared<WinDivertFlowSniffer>(io.get_executor(), callback);

  auto& controller = test::GetFakeWinDivertController();
  controller.Reset();

  bool opened = false;
  bool closed = false;
  HANDLE openedHandle = INVALID_HANDLE_VALUE;

  controller.SetOpenCallback([&](HANDLE handle, const char* filter, WINDIVERT_LAYER layer) {
    opened = true;
    openedHandle = handle;
    EXPECT_EQ(layer, WINDIVERT_LAYER_FLOW);
  });

  controller.SetCloseCallback([&](HANDLE handle) {
    closed = true;
    EXPECT_EQ(handle, openedHandle);
  });

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto err = co_await sniffer->Start();
    EXPECT_FALSE(err);
    EXPECT_TRUE(opened);

    err = co_await sniffer->Stop();
    EXPECT_FALSE(err);
    EXPECT_TRUE(closed);
    co_return;
  });

  RunEventLoop(io);
}

TEST(WinDivertFlowSnifferTest, FlowEventsTcpUdpIcmp) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  MockFlowSnifferCallback callback;
  auto sniffer = std::make_shared<WinDivertFlowSniffer>(io.get_executor(), callback);

  auto& controller = test::GetFakeWinDivertController();
  controller.Reset();

  HANDLE targetHandle = INVALID_HANDLE_VALUE;
  controller.SetOpenCallback([&](HANDLE handle, const char* filter, WINDIVERT_LAYER layer) { targetHandle = handle; });

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto err = co_await sniffer->Start();
    EXPECT_FALSE(err);
    if (targetHandle == INVALID_HANDLE_VALUE) {
      ADD_FAILURE() << "targetHandle is invalid";
      co_return;
    }

    // 1. TCP IPv4 Established
    {
      auto local = boost::asio::ip::make_address("192.168.1.100");
      auto remote = boost::asio::ip::make_address("8.8.8.8");
      PushFlowEvent(controller, targetHandle, WINDIVERT_EVENT_FLOW_ESTABLISHED, IPPROTO_TCP, local, 12345, remote, 443,
                    1111, false);

      boost::asio::steady_timer timer(io.get_executor());
      timer.expires_after(std::chrono::milliseconds(10));
      co_await timer.async_wait(Omni::Fiber::AsioUseFiber);

      if (callback.EstablishedEvents.size() != 1) {
        ADD_FAILURE() << "Expected 1 established event, got " << callback.EstablishedEvents.size();
        co_await sniffer->Stop();
        co_return;
      }
      EXPECT_EQ(callback.EstablishedEvents[0].Pid, 1111);
      auto key = std::get<ConnectionTracker::Ip4TcpKey>(callback.EstablishedEvents[0].Conn);
      EXPECT_EQ(key.LocalAddress, local.to_v4());
      EXPECT_EQ(key.LocalPort, 12345);
      EXPECT_EQ(key.RemoteAddress, remote.to_v4());
      EXPECT_EQ(key.RemotePort, 443);
    }

    // 2. TCP IPv6 Established
    {
      auto local = boost::asio::ip::make_address("fe80::1");
      auto remote = boost::asio::ip::make_address("2001:4860:4860::8888");
      PushFlowEvent(controller, targetHandle, WINDIVERT_EVENT_FLOW_ESTABLISHED, IPPROTO_TCP, local, 54321, remote, 80,
                    2222, true);

      boost::asio::steady_timer timer(io.get_executor());
      timer.expires_after(std::chrono::milliseconds(10));
      co_await timer.async_wait(Omni::Fiber::AsioUseFiber);

      if (callback.EstablishedEvents.size() != 2) {
        ADD_FAILURE() << "Expected 2 established events, got " << callback.EstablishedEvents.size();
        co_await sniffer->Stop();
        co_return;
      }
      EXPECT_EQ(callback.EstablishedEvents[1].Pid, 2222);
      auto key = std::get<ConnectionTracker::Ip6TcpKey>(callback.EstablishedEvents[1].Conn);
      EXPECT_EQ(key.LocalAddress, local.to_v6());
      EXPECT_EQ(key.LocalPort, 54321);
      EXPECT_EQ(key.RemoteAddress, remote.to_v6());
      EXPECT_EQ(key.RemotePort, 80);
    }

    // 3. UDP IPv4 Established
    {
      auto local = boost::asio::ip::make_address("10.0.0.5");
      auto remote = boost::asio::ip::make_address("1.1.1.1");
      PushFlowEvent(controller, targetHandle, WINDIVERT_EVENT_FLOW_ESTABLISHED, IPPROTO_UDP, local, 9999, remote, 53,
                    3333, false);

      boost::asio::steady_timer timer(io.get_executor());
      timer.expires_after(std::chrono::milliseconds(10));
      co_await timer.async_wait(Omni::Fiber::AsioUseFiber);

      if (callback.EstablishedEvents.size() != 3) {
        ADD_FAILURE() << "Expected 3 established events, got " << callback.EstablishedEvents.size();
        co_await sniffer->Stop();
        co_return;
      }
      EXPECT_EQ(callback.EstablishedEvents[2].Pid, 3333);
      auto key = std::get<ConnectionTracker::Ip4UdpKey>(callback.EstablishedEvents[2].Conn);
      EXPECT_EQ(key.LocalAddress, local.to_v4());
      EXPECT_EQ(key.LocalPort, 9999);
      EXPECT_EQ(key.RemoteAddress, remote.to_v4());
      EXPECT_EQ(key.RemotePort, 53);
    }

    // 4. UDP IPv6 Established
    {
      auto local = boost::asio::ip::make_address("2001:db8::1");
      auto remote = boost::asio::ip::make_address("2001:db8::2");
      PushFlowEvent(controller, targetHandle, WINDIVERT_EVENT_FLOW_ESTABLISHED, IPPROTO_UDP, local, 8888, remote, 9999,
                    4444, true);

      boost::asio::steady_timer timer(io.get_executor());
      timer.expires_after(std::chrono::milliseconds(10));
      co_await timer.async_wait(Omni::Fiber::AsioUseFiber);

      if (callback.EstablishedEvents.size() != 4) {
        ADD_FAILURE() << "Expected 4 established events, got " << callback.EstablishedEvents.size();
        co_await sniffer->Stop();
        co_return;
      }
      EXPECT_EQ(callback.EstablishedEvents[3].Pid, 4444);
      auto key = std::get<ConnectionTracker::Ip6UdpKey>(callback.EstablishedEvents[3].Conn);
      EXPECT_EQ(key.LocalAddress, local.to_v6());
      EXPECT_EQ(key.LocalPort, 8888);
      EXPECT_EQ(key.RemoteAddress, remote.to_v6());
      EXPECT_EQ(key.RemotePort, 9999);
    }

    // 5. TCP IPv4 Deleted
    {
      auto local = boost::asio::ip::make_address("192.168.1.100");
      auto remote = boost::asio::ip::make_address("8.8.8.8");
      PushFlowEvent(controller, targetHandle, WINDIVERT_EVENT_FLOW_DELETED, IPPROTO_TCP, local, 12345, remote, 443, 0,
                    false);

      boost::asio::steady_timer timer(io.get_executor());
      timer.expires_after(std::chrono::milliseconds(10));
      co_await timer.async_wait(Omni::Fiber::AsioUseFiber);

      if (callback.DeletedEvents.size() != 1) {
        ADD_FAILURE() << "Expected 1 deleted event, got " << callback.DeletedEvents.size();
        co_await sniffer->Stop();
        co_return;
      }
      auto key = std::get<ConnectionTracker::Ip4TcpKey>(callback.DeletedEvents[0].Conn);
      EXPECT_EQ(key.LocalAddress, local.to_v4());
      EXPECT_EQ(key.LocalPort, 12345);
      EXPECT_EQ(key.RemoteAddress, remote.to_v4());
      EXPECT_EQ(key.RemotePort, 443);
    }

    // 6. ICMP IPv4 Established (LocalPort acts as ICMP ID)
    {
      auto local = boost::asio::ip::make_address("10.0.0.5");
      auto remote = boost::asio::ip::make_address("8.8.8.8");
      PushFlowEvent(controller, targetHandle, WINDIVERT_EVENT_FLOW_ESTABLISHED, IPPROTO_ICMP, local, 500, remote, 0,
                    5555, false);

      boost::asio::steady_timer timer(io.get_executor());
      timer.expires_after(std::chrono::milliseconds(10));
      co_await timer.async_wait(Omni::Fiber::AsioUseFiber);

      if (callback.EstablishedEvents.size() != 5) {
        ADD_FAILURE() << "Expected 5 established events, got " << callback.EstablishedEvents.size();
        co_await sniffer->Stop();
        co_return;
      }
      EXPECT_EQ(callback.EstablishedEvents[4].Pid, 5555);
      auto key = std::get<ConnectionTracker::IcmpKey>(callback.EstablishedEvents[4].Conn);
      EXPECT_EQ(key.LocalAddress, local.to_v4());
      EXPECT_EQ(key.Id, 0);
      EXPECT_EQ(key.RemoteAddress, remote.to_v4());
    }

    // 7. ICMPv6 Established
    {
      auto local = boost::asio::ip::make_address("fe80::1");
      auto remote = boost::asio::ip::make_address("fe80::2");
      PushFlowEvent(controller, targetHandle, WINDIVERT_EVENT_FLOW_ESTABLISHED, IPPROTO_ICMPV6, local, 600, remote, 0,
                    6666, true);

      boost::asio::steady_timer timer(io.get_executor());
      timer.expires_after(std::chrono::milliseconds(10));
      co_await timer.async_wait(Omni::Fiber::AsioUseFiber);

      if (callback.EstablishedEvents.size() != 6) {
        ADD_FAILURE() << "Expected 6 established events, got " << callback.EstablishedEvents.size();
        co_await sniffer->Stop();
        co_return;
      }
      EXPECT_EQ(callback.EstablishedEvents[5].Pid, 6666);
      auto key = std::get<ConnectionTracker::Icmp6Key>(callback.EstablishedEvents[5].Conn);
      EXPECT_EQ(key.LocalAddress, local.to_v6());
      EXPECT_EQ(key.Id, 0);
      EXPECT_EQ(key.RemoteAddress, remote.to_v6());
    }

    // 8. Ignored flow events (e.g. invalid layer, unknown protocol)
    {
      // Unknown protocol
      auto local = boost::asio::ip::make_address("192.168.1.100");
      auto remote = boost::asio::ip::make_address("8.8.8.8");
      PushFlowEvent(controller, targetHandle, WINDIVERT_EVENT_FLOW_ESTABLISHED, 255 /* unknown */, local, 12345, remote,
                    443, 7777, false);

      // Wrong Layer
      WINDIVERT_ADDRESS wrongLayerAddr{};
      wrongLayerAddr.Layer = WINDIVERT_LAYER_NETWORK;
      wrongLayerAddr.Event = WINDIVERT_EVENT_FLOW_ESTABLISHED;
      wrongLayerAddr.Flow.Protocol = IPPROTO_TCP;
      wrongLayerAddr.Flow.ProcessId = 8888;
      controller.PushRecvPacket(targetHandle, {}, wrongLayerAddr);

      boost::asio::steady_timer timer(io.get_executor());
      timer.expires_after(std::chrono::milliseconds(10));
      co_await timer.async_wait(Omni::Fiber::AsioUseFiber);

      // Callback count should remain the same
      EXPECT_EQ(callback.EstablishedEvents.size(), 6);
      EXPECT_EQ(callback.DeletedEvents.size(), 1);
    }

    co_await sniffer->Stop();
    co_return;
  });

  RunEventLoop(io);
}
