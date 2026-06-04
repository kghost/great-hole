#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "Coroutine.hpp"
#include "GetCurrentFiber.hpp"
#include "Manager.hpp"
#include "resolvers/ResolverCombinedEndpoint.hpp"
#include "resolvers/ResolverDnsService.hpp"
#include "resolvers/ResolverHelper.hpp"
#include "resolvers/ResolverIpDns.hpp"
#include "resolvers/ResolverNumberPort.hpp"
#include "resolvers/ResolverServicePort.hpp"
#include "resolvers/ResolverStaticIp.hpp"

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
    auto r1 = std::make_shared<ResolverStaticIp>(boost::asio::ip::make_address("127.0.0.1"));
    auto err1 = co_await r1->Start();
    EXPECT_FALSE(err1);
    if (!err1) {
      auto addr = r1->GetResolverResult();
      EXPECT_EQ(addr.to_string(), "127.0.0.1");
    }

    auto r2 = std::make_shared<ResolverStaticIp>(boost::asio::ip::make_address("::1"));
    auto err2 = co_await r2->Start();
    EXPECT_FALSE(err2);
    if (!err2) {
      auto addr = r2->GetResolverResult();
      EXPECT_EQ(addr.to_string(), "::1");
    }

    co_await r1->Stop();
    co_await r2->Stop();
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
    auto r1 = std::make_shared<ResolverNumberPort>(8080);
    auto err1 = co_await r1->Start();
    EXPECT_FALSE(err1);
    if (!err1) {
      EXPECT_EQ(r1->GetResolverResult(), 8080);
    }

    auto r2 = std::make_shared<ResolverNumberPort>(1234);
    auto err2 = co_await r2->Start();
    EXPECT_FALSE(err2);
    if (!err2) {
      EXPECT_EQ(r2->GetResolverResult(), 1234);
    }

    auto r3 = std::make_shared<ResolverServicePort>("http");
    auto err3 = co_await r3->Start();
    EXPECT_FALSE(err3);
    if (!err3) {
      EXPECT_EQ(r3->GetResolverResult(), 80);
    }

    co_await r1->Stop();
    co_await r2->Stop();
    co_await r3->Stop();
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
    auto r1 = std::make_shared<ResolverServicePort>("abc");
    auto err1 = co_await r1->Start();
    EXPECT_TRUE(err1);

    auto r2 = std::make_shared<ResolverServicePort>("99999");
    auto err2 = co_await r2->Start();
    EXPECT_TRUE(err2);

    co_await r1->WaitService();
    co_await r2->WaitService();
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
    auto r = std::make_shared<ResolverIpDns>(io.get_executor(), "localhost");
    auto err = co_await r->Start();
    // DNS resolving "localhost" should succeed on any machine
    EXPECT_FALSE(err);
    if (!err) {
      auto addr = r->GetResolverResult();
      EXPECT_FALSE(addr.is_unspecified());
    }

    co_await r->Stop();
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
    auto ipResolver = std::make_shared<ResolverStaticIp>(boost::asio::ip::make_address("127.0.0.1"));
    auto portResolver = std::make_shared<ResolverNumberPort>(9090);
    auto r = std::make_shared<ResolverCombinedEndpoint>(ipResolver, portResolver);

    auto err = co_await r->Start();
    EXPECT_FALSE(err);
    if (!err) {
      auto endpoint = r->GetResolverResult();
      EXPECT_EQ(endpoint.address().to_string(), "127.0.0.1");
      EXPECT_EQ(endpoint.port(), 9090);
    }

    co_await r->Stop();
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

    auto r = std::make_shared<ResolverDnsService>("_nonexistent_service._tcp.example.invalid", mockedResolveFor);
    auto err = co_await r->Start();
    // Resolving a non-existent SRV record should fail with host_not_found
    EXPECT_TRUE(err);
    EXPECT_EQ(err, make_error_code(boost::asio::error::host_not_found));

    co_await r->WaitService();
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

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto& current = co_await Omni::Fiber::GetCurrentFiber();
    // ResolverIpDns cancellation (instant cancellation)
    auto dnsResolver = std::make_shared<ResolverIpDns>(io.get_executor(), "nonexistent.example.invalid");
    auto resolveFiber = current.Spawn("resolve", [&]() -> Omni::Fiber::Coroutine<void> {
      auto err = co_await dnsResolver->Start();
      EXPECT_EQ(err, make_error_code(boost::asio::error::operation_aborted));
      co_await dnsResolver->WaitService();
      co_return;
    });
    auto resolveFiberCancel = current.Spawn("resolve_cancel", [&]() -> Omni::Fiber::Coroutine<void> {
      co_await dnsResolver->Stop();
      co_return;
    });
    co_await current.Join(resolveFiber);
    co_await current.Join(resolveFiberCancel);

    auto mockedResolveFor = MockResolveeFor(io.get_executor(), "", ResolveFor::Protocol::Udp);
    // ResolverDnsService cancellation (instant cancellation)
    auto srvResolver = std::make_shared<ResolverDnsService>("_sip._udp.nonexistent.example.invalid", mockedResolveFor);
    auto srvFiber = current.Spawn("srv_resolve", [&]() -> Omni::Fiber::Coroutine<void> {
      auto err = co_await srvResolver->Start();
      EXPECT_EQ(err, make_error_code(boost::asio::error::operation_aborted));
      co_await srvResolver->WaitService();
      co_return;
    });
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

    // 1. Test FindResolverIp
    auto ipRes1 = FindResolverIp("127.0.0.1", mockedResolveFor);
    auto result1 = co_await ipRes1->Resolve();
    EXPECT_TRUE(result1.has_value());
    if (result1.has_value()) {
      auto addr = result1.value();
      EXPECT_EQ(addr.to_string(), "127.0.0.1");
    }

    auto ipRes2 = FindResolverIp("localhost", mockedResolveFor);
    auto result2 = co_await ipRes2->Resolve();
    EXPECT_TRUE(result2.has_value());
    if (result2.has_value()) {
      EXPECT_FALSE(result2.value().is_unspecified());
    }

    // 2. Test FindResolverPort
    auto portRes1 = FindResolverPort("8080", mockedResolveFor);
    auto result3_1 = co_await portRes1->Resolve();
    EXPECT_TRUE(result3_1.has_value());
    if (result3_1.has_value()) {
      EXPECT_EQ(result3_1.value(), 8080);
    }

    auto portRes2 = FindResolverPort("http", mockedResolveFor);
    auto result3_2 = co_await portRes2->Resolve();
    EXPECT_TRUE(result3_2.has_value());
    if (result3_2.has_value()) {
      EXPECT_EQ(result3_2.value(), 80);
    }

    // 3. Test FindResolverEndpoint (Combined IPv4)
    auto epRes1 = FindResolverEndpoint("127.0.0.1:9090", mockedResolveFor);
    auto result4 = co_await epRes1->Resolve();
    EXPECT_TRUE(result4.has_value());
    if (result4.has_value()) {
      auto endpoint = result4.value();
      EXPECT_EQ(endpoint.address().to_string(), "127.0.0.1");
      EXPECT_EQ(endpoint.port(), 9090);
    }

    // 4. Test FindResolverEndpoint (Combined IPv6)
    auto epRes2 = FindResolverEndpoint("[::1]:9999", mockedResolveFor);
    auto result5 = co_await epRes2->Resolve();
    EXPECT_TRUE(result5.has_value());
    if (result5.has_value()) {
      auto endpoint = result5.value();
      EXPECT_EQ(endpoint.address().to_string(), "::1");
      EXPECT_EQ(endpoint.port(), 9999);
    }

    // 5. Test FindResolverEndpoint (SRV Dns Service)
    auto epRes3 = FindResolverEndpoint("example.invalid", mockedResolveFor);
    auto result6 = co_await epRes3->Resolve();
    // Non-existent SRV record should fail with host_not_found
    EXPECT_FALSE(result6.has_value());

    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}
