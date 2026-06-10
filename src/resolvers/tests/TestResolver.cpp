#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "GetCurrentFiber.hpp"
#include "Manager.hpp"
#include "ResolverCombinedEndpoint.hpp"
#include "ResolverDnsService.hpp"
#include "ResolverHelper.hpp"
#include "ResolverIpDns.hpp"
#include "ResolverNumberPort.hpp"
#include "ResolverServicePort.hpp"
#include "ResolverStaticIp.hpp"
#include "Utils.hpp"
#include "Yield.hpp"

using namespace gh;

namespace {

class MockResolveeFor : public ResolveFor {
public:
  MockResolveeFor(boost::asio::any_io_executor executor, std::string service, Protocol protocol)
      : _Executor(executor), _Service(service), _Protocol(protocol) {}
  ~MockResolveeFor() override = default;

  boost::asio::any_io_executor GetExecutor() override { return _Executor; }
  std::string GetService() override { return _Service; }
  Protocol GetProtocol() override { return _Protocol; }

  boost::asio::any_io_executor _Executor;
  std::string _Service;
  Protocol _Protocol;
};

void RunEventLoop(boost::asio::io_context& io) {
  io.restart();
  io.run();
}

} // namespace

TEST(ResolverTest, StaticIpResolverSuccess) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto& current = co_await Omni::Fiber::GetCurrentFiber();
    Cancel c;
    auto r1 = std::make_shared<ResolverStaticIp>(MapToV6(boost::asio::ip::make_address("127.0.0.1")));
    auto res1 = co_await r1->Resolve(c);
    EXPECT_TRUE(res1.has_value());
    if (res1.has_value()) {
      EXPECT_EQ(res1.value().to_string(), "::ffff:127.0.0.1");
    }

    auto r2 = std::make_shared<ResolverStaticIp>(boost::asio::ip::address_v6::loopback());
    auto res2 = co_await r2->Resolve(c);
    EXPECT_TRUE(res2.has_value());
    if (res2.has_value()) {
      EXPECT_EQ(res2.value().to_string(), "::1");
    }

    co_await current.WaitAll();

    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(ResolverTest, StaticPortResolverSuccess) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto& current = co_await Omni::Fiber::GetCurrentFiber();
    Cancel c;
    auto r1 = std::make_shared<ResolverNumberPort>(8080);
    auto res1 = co_await r1->Resolve(c);
    EXPECT_TRUE(res1.has_value());
    if (res1.has_value()) {
      EXPECT_EQ(res1.value(), 8080);
    }

    auto r2 = std::make_shared<ResolverNumberPort>(1234);
    auto res2 = co_await r2->Resolve(c);
    EXPECT_TRUE(res2.has_value());
    if (res2.has_value()) {
      EXPECT_EQ(res2.value(), 1234);
    }

    auto r3 = std::make_shared<ResolverServicePort>("http");
    auto res3 = co_await r3->Resolve(c);
    EXPECT_TRUE(res3.has_value());
    if (res3.has_value()) {
      EXPECT_EQ(res3.value(), 80);
    }

    co_await current.WaitAll();

    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(ResolverTest, StaticPortResolverFailure) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto& current = co_await Omni::Fiber::GetCurrentFiber();
    Cancel c;
    auto r1 = std::make_shared<ResolverServicePort>("abc");
    auto res1 = co_await r1->Resolve(c);
    EXPECT_FALSE(res1.has_value());

    auto r2 = std::make_shared<ResolverServicePort>("99999");
    auto res2 = co_await r2->Resolve(c);
    EXPECT_FALSE(res2.has_value());

    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(ResolverTest, StaticDnsResolverSuccess) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto& current = co_await Omni::Fiber::GetCurrentFiber();
    Cancel c;
    auto r = std::make_shared<ResolverIpDns>(io.get_executor(), "localhost");
    auto res = co_await r->Resolve(c);
    EXPECT_TRUE(res.has_value());
    if (res.has_value()) {
      EXPECT_FALSE(res.value().is_unspecified());
    }

    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(ResolverTest, ResolverEndpointSuccess) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto& current = co_await Omni::Fiber::GetCurrentFiber();
    Cancel c;
    auto ipResolver = std::make_shared<ResolverStaticIp>(MapToV6(boost::asio::ip::make_address("127.0.0.1")));
    auto portResolver = std::make_shared<ResolverNumberPort>(9090);
    auto r = std::make_shared<ResolverCombinedEndpoint>(ipResolver, portResolver);

    auto res = co_await r->Resolve(c);
    EXPECT_TRUE(res.has_value());
    if (res.has_value()) {
      EXPECT_EQ(res.value().address().to_string(), "::ffff:127.0.0.1");
      EXPECT_EQ(res.value().port(), 9090);
    }

    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(ResolverTest, DnsServiceResolverNonExistent) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto& current = co_await Omni::Fiber::GetCurrentFiber();
    auto mockedResolveFor = MockResolveeFor(io.get_executor(), "", ResolveFor::Protocol::Udp);

    Cancel c;
    auto r = std::make_shared<ResolverDnsService>("_nonexistent_service._tcp.example.invalid", mockedResolveFor);
    auto res = co_await r->Resolve(c);
    EXPECT_FALSE(res.has_value());
    if (!res.has_value()) {
      EXPECT_EQ(res.error(), make_error_code(boost::asio::error::host_not_found));
    }

    co_await current.WaitAll();
    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}

TEST(ResolverTest, ResolverCancellation) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  ::setenv("ARES_SERVERS", "127.0.0.99", 1);
  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto& current = co_await Omni::Fiber::GetCurrentFiber();

    // ResolverIpDns cancellation
    auto dnsResolver = std::make_shared<ResolverIpDns>(io.get_executor(), "nonexistent.example.invalid");
    auto resolveFiber = current.Spawn("resolve", [&]() -> Omni::Fiber::Coroutine<void> {
      Cancel c;
      auto res = co_await dnsResolver->Resolve(c);
      EXPECT_FALSE(res.has_value());
      if (!res.has_value()) {
        auto err = ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
        EXPECT_EQ(res.error(), err);
      }
      co_return;
    });
    co_await Omni::Fiber::Yield();
    auto resolveFiberCancel = current.Spawn("resolve_cancel", [&]() -> Omni::Fiber::Coroutine<void> {
      co_await dnsResolver->Stop();
      co_return;
    });
    co_await current.Join(resolveFiber);
    co_await current.Join(resolveFiberCancel);

    auto mockedResolveFor = MockResolveeFor(io.get_executor(), "", ResolveFor::Protocol::Udp);
    // ResolverDnsService cancellation
    auto srvResolver = std::make_shared<ResolverDnsService>("_sip._udp.nonexistent.example.invalid", mockedResolveFor);
    auto srvFiber = current.Spawn("srv_resolve", [&]() -> Omni::Fiber::Coroutine<void> {
      Cancel c;
      auto res = co_await srvResolver->Resolve(c);
      EXPECT_FALSE(res.has_value());
      if (!res.has_value()) {
        auto err = ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
        EXPECT_EQ(res.error(), err);
      }
      co_return;
    });
    co_await Omni::Fiber::Yield();
    auto srvFiberCancel = current.Spawn("srv_resolve_cancel", [&]() -> Omni::Fiber::Coroutine<void> {
      co_await srvResolver->Stop();
      co_return;
    });
    co_await current.Join(srvFiber);
    co_await current.Join(srvFiberCancel);

    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  ::unsetenv("ARES_SERVERS");
  EXPECT_TRUE(testPassed);
}

TEST(ResolverTest, ResolverHelperTest) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto& current = co_await Omni::Fiber::GetCurrentFiber();
    auto mockedResolveFor = MockResolveeFor(io.get_executor(), "", ResolveFor::Protocol::Udp);
    Cancel c;

    // 1. Test FindResolverIp
    auto ipRes1 = FindResolverIp("127.0.0.1", mockedResolveFor);
    auto result1 = co_await ipRes1->Resolve(c);
    EXPECT_TRUE(result1.has_value());
    if (result1.has_value()) {
      auto addr = result1.value();
      EXPECT_EQ(addr.to_string(), "::ffff:127.0.0.1");
    }

    auto ipRes2 = FindResolverIp("localhost", mockedResolveFor);
    auto result2 = co_await ipRes2->Resolve(c);
    EXPECT_TRUE(result2.has_value());
    if (result2.has_value()) {
      EXPECT_FALSE(result2.value().is_unspecified());
    }

    // 2. Test FindResolverPort
    auto portRes1 = FindResolverPort("8080", mockedResolveFor);
    auto result3_1 = co_await portRes1->Resolve(c);
    EXPECT_TRUE(result3_1.has_value());
    if (result3_1.has_value()) {
      EXPECT_EQ(result3_1.value(), 8080);
    }

    auto portRes2 = FindResolverPort("http", mockedResolveFor);
    auto result3_2 = co_await portRes2->Resolve(c);
    EXPECT_TRUE(result3_2.has_value());
    if (result3_2.has_value()) {
      EXPECT_EQ(result3_2.value(), 80);
    }

    // 3. Test FindResolverEndpoint (Combined IPv4)
    auto epRes1 = FindResolverEndpoint("127.0.0.1:9090", mockedResolveFor);
    auto result4 = co_await epRes1->Resolve(c);
    EXPECT_TRUE(result4.has_value());
    if (result4.has_value()) {
      auto endpoint = result4.value();
      EXPECT_EQ(endpoint.address().to_string(), "::ffff:127.0.0.1");
      EXPECT_EQ(endpoint.port(), 9090);
    }

    // 4. Test FindResolverEndpoint (Combined IPv6)
    auto epRes2 = FindResolverEndpoint("[::1]:9999", mockedResolveFor);
    auto result5 = co_await epRes2->Resolve(c);
    EXPECT_TRUE(result5.has_value());
    if (result5.has_value()) {
      auto endpoint = result5.value();
      EXPECT_EQ(endpoint.address().to_string(), "::1");
      EXPECT_EQ(endpoint.port(), 9999);
    }

    // 5. Test FindResolverEndpoint (SRV Dns Service)
    auto epRes3 = FindResolverEndpoint("example.invalid", mockedResolveFor);
    auto result6 = co_await epRes3->Resolve(c);
    // Non-existent SRV record should fail with host_not_found
    EXPECT_FALSE(result6.has_value());

    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}
