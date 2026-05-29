#include <memory>
#include <string>
#include <vector>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "Coroutine.hpp"
#include "GetCurrentFiber.hpp"
#include "Manager.hpp"
#include "Resolver.hpp"
#include "ResolverHelper.hpp"

using namespace gh;

namespace {

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
    auto r1 = std::make_shared<ResolverStaticIp>("127.0.0.1");
    auto err1 = co_await r1->Start();
    EXPECT_FALSE(err1);
    if (!err1) {
      auto addrs = r1->GetAddresses();
      EXPECT_EQ(addrs.size(), 1);
      if (addrs.size() == 1) {
        EXPECT_EQ(addrs[0].to_string(), "127.0.0.1");
      }
    }

    auto r2 = std::make_shared<ResolverStaticIp>("::1");
    auto err2 = co_await r2->Start();
    EXPECT_FALSE(err2);
    if (!err2) {
      auto addrs = r2->GetAddresses();
      EXPECT_EQ(addrs.size(), 1);
      if (addrs.size() == 1) {
        EXPECT_EQ(addrs[0].to_string(), "::1");
      }
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

TEST(ResolverTest, StaticIpResolverFailure) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto& current = co_await Omni::Fiber::GetCurrentFiber();
    auto r = std::make_shared<ResolverStaticIp>("invalid_ip");
    auto err = co_await r->Start();
    EXPECT_TRUE(err);

    co_await r->Stop();
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
    auto r1 = std::make_shared<ResolverStaticPort>("8080");
    auto err1 = co_await r1->Start();
    EXPECT_FALSE(err1);
    if (!err1) {
      EXPECT_EQ(r1->GetPort(), 8080);
    }

    auto r2 = std::make_shared<ResolverStaticPort>(1234);
    auto err2 = co_await r2->Start();
    EXPECT_FALSE(err2);
    if (!err2) {
      EXPECT_EQ(r2->GetPort(), 1234);
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

TEST(ResolverTest, StaticPortResolverFailure) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto& current = co_await Omni::Fiber::GetCurrentFiber();
    auto r1 = std::make_shared<ResolverStaticPort>("abc");
    auto err1 = co_await r1->Start();
    EXPECT_TRUE(err1);

    auto r2 = std::make_shared<ResolverStaticPort>("99999");
    auto err2 = co_await r2->Start();
    EXPECT_TRUE(err2);

    co_await r1->Stop();
    co_await r2->Stop();
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
    auto r = std::make_shared<ResolverStaticDns>(io, "localhost");
    auto err = co_await r->Start();
    // DNS resolving "localhost" should succeed on any machine
    EXPECT_FALSE(err);
    if (!err) {
      auto addrs = r->GetAddresses();
      EXPECT_FALSE(addrs.empty());
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
    auto ipResolver = std::make_shared<ResolverStaticIp>("127.0.0.1");
    auto portResolver = std::make_shared<ResolverStaticPort>("9090");
    auto r = std::make_shared<ResolverCombinedEndpoint>(io, ipResolver, portResolver);

    auto err = co_await r->Start();
    EXPECT_FALSE(err);
    if (!err) {
      auto endpoints = r->GetEndpoints();
      EXPECT_EQ(endpoints.size(), 1);
      if (endpoints.size() == 1) {
        EXPECT_EQ(endpoints[0].address().to_string(), "127.0.0.1");
        EXPECT_EQ(endpoints[0].port(), 9090);
      }
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
    auto r = std::make_shared<ResolverDnsService>(io, "_nonexistent_service._tcp.example.invalid");
    auto err = co_await r->Start();
    // Resolving a non-existent SRV record should fail with host_not_found
    EXPECT_TRUE(err);
    EXPECT_EQ(err, make_error_code(boost::asio::error::host_not_found));

    co_await r->Stop();
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

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto& current = co_await Omni::Fiber::GetCurrentFiber();

    // ResolverStaticDns cancellation (instant cancellation)
    auto dnsResolver = std::make_shared<ResolverStaticDns>(io, "nonexistent.example.invalid");
    auto resolveFiber = current.Spawn("resolve", [&]() -> Omni::Fiber::Coroutine<void> {
      auto err = co_await dnsResolver->Start();
      EXPECT_EQ(err, make_error_code(boost::asio::error::operation_aborted));
      co_await (co_await Omni::Fiber::GetCurrentFiber()).WaitAll();
      co_return;
    });
    co_await dnsResolver->Stop();
    co_await current.Join(resolveFiber);

    // ResolverDnsService cancellation (instant cancellation)
    auto srvResolver = std::make_shared<ResolverDnsService>(io, "_sip._udp.nonexistent.example.invalid");
    auto srvFiber = current.Spawn("srv_resolve", [&]() -> Omni::Fiber::Coroutine<void> {
      auto err = co_await srvResolver->Start();
      EXPECT_EQ(err, make_error_code(boost::asio::error::operation_aborted));
      co_await (co_await Omni::Fiber::GetCurrentFiber()).WaitAll();
      co_return;
    });
    co_await srvResolver->Stop();
    co_await current.Join(srvFiber);

    co_await current.WaitAll();

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

    // 1. Test FindResolverIp
    auto ipRes1 = FindResolverIp(io, "127.0.0.1");
    auto err1 = co_await ipRes1->Start();
    EXPECT_FALSE(err1);
    if (!err1) {
      auto addrs = ipRes1->GetAddresses();
      EXPECT_EQ(addrs.size(), 1);
      if (addrs.size() == 1) {
        EXPECT_EQ(addrs[0].to_string(), "127.0.0.1");
      }
    }

    auto ipRes2 = FindResolverIp(io, "localhost");
    auto err2 = co_await ipRes2->Start();
    EXPECT_FALSE(err2);
    if (!err2) {
      EXPECT_FALSE(ipRes2->GetAddresses().empty());
    }

    // 2. Test FindResolverPort
    auto portRes = FindResolverPort("8080");
    auto err3 = co_await portRes->Start();
    EXPECT_FALSE(err3);
    if (!err3) {
      EXPECT_EQ(portRes->GetPort(), 8080);
    }

    // 3. Test FindResolverEndpoint (Combined IPv4)
    auto epRes1 = FindResolverEndpoint(io, "127.0.0.1:9090");
    auto err4 = co_await epRes1->Start();
    EXPECT_FALSE(err4);
    if (!err4) {
      auto endpoints = epRes1->GetEndpoints();
      EXPECT_EQ(endpoints.size(), 1);
      if (endpoints.size() == 1) {
        EXPECT_EQ(endpoints[0].address().to_string(), "127.0.0.1");
        EXPECT_EQ(endpoints[0].port(), 9090);
      }
    }

    // 4. Test FindResolverEndpoint (Combined IPv6)
    auto epRes2 = FindResolverEndpoint(io, "[::1]:9999");
    auto err5 = co_await epRes2->Start();
    EXPECT_FALSE(err5);
    if (!err5) {
      auto endpoints = epRes2->GetEndpoints();
      EXPECT_EQ(endpoints.size(), 1);
      if (endpoints.size() == 1) {
        EXPECT_EQ(endpoints[0].address().to_string(), "::1");
        EXPECT_EQ(endpoints[0].port(), 9999);
      }
    }

    // 5. Test FindResolverEndpoint (SRV Dns Service)
    auto epRes3 = FindResolverEndpoint(io, "example.invalid", "sip", "udp");
    auto err6 = co_await epRes3->Start();
    // Non-existent SRV record should fail with host_not_found
    EXPECT_TRUE(err6);
    EXPECT_EQ(err6, make_error_code(boost::asio::error::host_not_found));

    // Stop all
    co_await ipRes1->Stop();
    co_await ipRes2->Stop();
    co_await portRes->Stop();
    co_await epRes1->Stop();
    co_await epRes2->Stop();
    co_await epRes3->Stop();

    co_await current.WaitAll();

    testPassed = true;
    co_return;
  });

  RunEventLoop(io);
  EXPECT_TRUE(testPassed);
}
