#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "Coroutine.hpp"
#include "DnsForwarder.hpp"
#include "DnsPacket.hpp"
#include "DnsRouter.hpp"
#include "DnsUpstream.hpp"
#include "GetCurrentOmniFiber.hpp"
#include "Manager.hpp"

using namespace gh;
using namespace gh::dns;

TEST(DnsPacketTest, SerializeAndParseHeaderAndQuestion) {
  DnsPacket orig;
  orig.Header.Id = 0x1234;
  orig.Header.SetResponse(false);
  orig.Header.SetRCode(DnsRCode::NoError);

  DnsQuestion q;
  q.QName = "www.example.com";
  q.QType = static_cast<uint16_t>(DnsType::A);
  q.QClass = 1;
  orig.Questions.push_back(q);

  auto serialized = orig.Serialize();
  EXPECT_FALSE(serialized.empty());

  auto parsedRes = DnsPacket::Parse(serialized);
  ASSERT_TRUE(parsedRes.has_value());

  EXPECT_EQ(parsedRes->Header.Id, 0x1234);
  EXPECT_TRUE(parsedRes->Header.IsQuery());
  EXPECT_EQ(parsedRes->Header.GetRCode(), DnsRCode::NoError);
  ASSERT_EQ(parsedRes->Questions.size(), 1u);
  EXPECT_EQ(parsedRes->Questions[0].QName, "www.example.com");
  EXPECT_EQ(parsedRes->Questions[0].QType, static_cast<uint16_t>(DnsType::A));
}

TEST(DnsPacketTest, SerializeAndParseAnswerRecord) {
  DnsPacket orig;
  orig.Header.Id = 0x5678;
  orig.Header.SetResponse(true);

  DnsResourceRecord rr;
  rr.Name = "api.google.com";
  rr.Type = static_cast<uint16_t>(DnsType::A);
  rr.RClass = 1;
  rr.Ttl = 600;
  rr.RData = {8, 8, 8, 8};
  orig.Answers.push_back(rr);

  auto serialized = orig.Serialize();
  auto parsedRes = DnsPacket::Parse(serialized);
  ASSERT_TRUE(parsedRes.has_value());

  EXPECT_EQ(parsedRes->Header.Id, 0x5678);
  EXPECT_TRUE(parsedRes->Header.IsResponse());
  ASSERT_EQ(parsedRes->Answers.size(), 1u);
  EXPECT_EQ(parsedRes->Answers[0].Name, "api.google.com");
  EXPECT_EQ(parsedRes->Answers[0].Ttl, 600u);
  EXPECT_EQ(parsedRes->Answers[0].RData, (std::vector<uint8_t>{8, 8, 8, 8}));
}

TEST(DnsRouterTest, SuffixMatchingAndLongestMatchWins) {
  auto router = std::make_shared<DnsRouter>();
  boost::asio::io_context io;

  auto defaultClient = std::make_shared<DnsUpstream>(io.get_executor(), std::vector<boost::asio::ip::udp::endpoint>{});
  auto companyClient = std::make_shared<DnsUpstream>(io.get_executor(), std::vector<boost::asio::ip::udp::endpoint>{});
  auto subCompanyClient =
      std::make_shared<DnsUpstream>(io.get_executor(), std::vector<boost::asio::ip::udp::endpoint>{});

  router->SetDefaultRoute(defaultClient);
  router->AddRoute("company.com", companyClient);
  router->AddRoute("internal.company.com", subCompanyClient);

  // Exact & Suffix matching
  EXPECT_EQ(router->Route("foo.bar.org"), defaultClient);
  EXPECT_EQ(router->Route("company.com"), companyClient);
  EXPECT_EQ(router->Route("web.company.com"), companyClient);
  EXPECT_EQ(router->Route("internal.company.com"), subCompanyClient);
  EXPECT_EQ(router->Route("api.internal.company.com"), subCompanyClient);

  // Remove route
  router->RemoveRoute("internal.company.com");
  EXPECT_EQ(router->Route("api.internal.company.com"), companyClient);

  // Test weak_ptr expiration handling
  subCompanyClient.reset();
  companyClient.reset();
  EXPECT_EQ(router->Route("company.com"), defaultClient);

  defaultClient.reset();
  EXPECT_EQ(router->Route("foo.bar.org"), std::nullopt);
}

TEST(DnsUpstreamTest, EphemeralLocalPortBinding) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto client = std::make_shared<DnsUpstream>(
        io.get_executor(), std::vector<boost::asio::ip::udp::endpoint>{
                               boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::loopback(), 53053)});

    auto errStart = co_await client->Start();
    EXPECT_FALSE(errStart);

    uint16_t port = client->GetLocalPort();
    EXPECT_GT(port, 0u);

    co_await client->Stop();
    testPassed = true;
    co_return;
  });

  io.run();
  EXPECT_TRUE(testPassed);
}

TEST(DnsForwarderIntegrationTest, EndToEndForwardingAndRouting) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    // 1. Setup mock upstream DNS server on ephemeral port
    boost::asio::ip::udp::endpoint mockUpstreamBindEp(boost::asio::ip::address_v4::loopback(), 0);
    auto mockSocket = std::make_shared<boost::asio::ip::udp::socket>(io.get_executor());
    mockSocket->open(mockUpstreamBindEp.protocol());
    mockSocket->bind(mockUpstreamBindEp);
    boost::asio::ip::udp::endpoint mockUpstreamEp = mockSocket->local_endpoint();

    // Spawn mock upstream echo coroutine
    auto mockUpstreamFiber =
        (co_await Omni::Fiber::GetCurrentOmniFiber())
            .Spawn("MockUpstream", [mockSocket]() -> Omni::Fiber::Coroutine<void> {
              std::vector<uint8_t> buf(2048);
              boost::asio::ip::udp::endpoint clientEp;

              auto [ec, n] = co_await mockSocket->async_receive_from(boost::asio::buffer(buf), clientEp,
                                                                     Omni::Fiber::AsioUseFiber);

              if (!ec) {
                auto req = DnsPacket::Parse(std::span<const uint8_t>(buf.data(), n));
                if (req) {
                  DnsPacket resp;
                  resp.Header.Id = req->Header.Id;
                  resp.Header.SetResponse(true);
                  resp.Questions = req->Questions;

                  DnsResourceRecord rr;
                  rr.Name = req->GetPrimaryQName();
                  rr.Type = static_cast<uint16_t>(DnsType::A);
                  rr.Ttl = 120;
                  rr.RData = {192, 168, 1, 100};
                  resp.Answers.push_back(rr);

                  auto wire = resp.Serialize();
                  co_await mockSocket->async_send_to(boost::asio::buffer(wire), clientEp, Omni::Fiber::AsioUseFiber);
                }
              }
            });

    boost::asio::ip::udp::endpoint tempEp(boost::asio::ip::address_v4::loopback(), 0);
    boost::asio::ip::udp::socket tempSock(io.get_executor());
    tempSock.open(tempEp.protocol());
    tempSock.bind(tempEp);
    boost::asio::ip::udp::endpoint forwarderBindEp = tempSock.local_endpoint();
    tempSock.close();

    auto forwarder = std::make_shared<DnsForwarder>(io.get_executor());
    auto errStart = co_await forwarder->Start();
    EXPECT_FALSE(errStart);

    auto listenerRes = co_await forwarder->AddListener(forwarderBindEp);
    EXPECT_TRUE(listenerRes.has_value());

    boost::asio::ip::udp::endpoint forwarderListenEp = forwarderBindEp;

    // 3. Register client and route
    auto upstreamClient = co_await forwarder->AddUpstream({mockUpstreamEp});
    EXPECT_TRUE(upstreamClient.has_value());
    forwarder->SetDefaultRoute(upstreamClient.value());

    // 4. Send test query to DnsForwarder listener
    boost::asio::ip::udp::socket testClientSocket(io.get_executor());
    testClientSocket.open(boost::asio::ip::udp::v4());
    testClientSocket.bind(boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::loopback(), 0));

    DnsPacket queryPacket;
    queryPacket.Header.Id = 0xABCD;
    DnsQuestion q;
    q.QName = "test.company.com";
    q.QType = static_cast<uint16_t>(DnsType::A);
    queryPacket.Questions.push_back(q);

    auto queryWire = queryPacket.Serialize();
    co_await testClientSocket.async_send_to(boost::asio::buffer(queryWire), forwarderListenEp,
                                            Omni::Fiber::AsioUseFiber);

    // 5. Receive response from DnsForwarder
    std::vector<uint8_t> respBuf(2048);
    boost::asio::ip::udp::endpoint senderEp;
    auto [ecRecv, respLen] =
        co_await testClientSocket.async_receive_from(boost::asio::buffer(respBuf), senderEp, Omni::Fiber::AsioUseFiber);

    if (ecRecv) {
      BOOST_LOG_TRIVIAL(error) << "testClientSocket receive failed: " << ecRecv.message();
    }
    EXPECT_FALSE(ecRecv);
    auto respPacket = DnsPacket::Parse(std::span<const uint8_t>(respBuf.data(), respLen));
    EXPECT_TRUE(respPacket.has_value());
    if (respPacket) {
      EXPECT_EQ(respPacket->Header.Id, 0xABCD);
      EXPECT_TRUE(respPacket->Header.IsResponse());
      EXPECT_EQ(respPacket->Answers.size(), 1u);
      if (!respPacket->Answers.empty()) {
        EXPECT_EQ(respPacket->Answers[0].Name, "test.company.com");
        EXPECT_EQ(respPacket->Answers[0].RData, (std::vector<uint8_t>{192, 168, 1, 100}));
      }
    }

    // Cleanup
    mockSocket->close();
    co_await (co_await Omni::Fiber::GetCurrentOmniFiber()).Join(mockUpstreamFiber);
    testClientSocket.close();
    co_await forwarder->Stop();

    testPassed = true;
    co_return;
  });

  io.run();
  EXPECT_TRUE(testPassed);
}

TEST(DnsForwarderTest, DynamicAddAndRemoveUpstream) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto forwarder = std::make_shared<DnsForwarder>(io.get_executor());

    auto errStart = co_await forwarder->Start();
    EXPECT_FALSE(errStart);

    // Dynamic add upstream while running
    auto result = co_await forwarder->AddUpstream(
        {boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::loopback(), 53053)});
    EXPECT_TRUE(result.has_value());
    auto upstream = result.value().lock();
    EXPECT_TRUE(upstream);
    EXPECT_EQ(upstream->GetState(), ServiceBase::State::kRunning);

    // Dynamic remove upstream while running
    co_await forwarder->RemoveUpstream(upstream);
    EXPECT_EQ(upstream->GetState(), ServiceBase::State::kNone);

    co_await forwarder->Stop();
    testPassed = true;
    co_return;
  });

  io.run();
  EXPECT_TRUE(testPassed);
}

TEST(DnsForwarderTest, DynamicAddAndRemoveListener) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto forwarder = std::make_shared<DnsForwarder>(io.get_executor());

    auto errStart = co_await forwarder->Start();
    EXPECT_FALSE(errStart);

    boost::asio::ip::udp::endpoint tempEp(boost::asio::ip::address_v4::loopback(), 0);
    boost::asio::ip::udp::socket tempSock(io.get_executor());
    tempSock.open(tempEp.protocol());
    tempSock.bind(tempEp);
    boost::asio::ip::udp::endpoint bindEp = tempSock.local_endpoint();
    tempSock.close();

    // Dynamic add listener while running
    auto result = co_await forwarder->AddListener(bindEp);
    EXPECT_TRUE(result.has_value());
    auto listener = result.value().lock();
    EXPECT_TRUE(listener);
    EXPECT_EQ(listener->GetState(), ServiceBase::State::kRunning);

    // Dynamic remove listener while running
    co_await forwarder->RemoveListener(result.value());
    EXPECT_EQ(listener->GetState(), ServiceBase::State::kNone);

    co_await forwarder->Stop();
    testPassed = true;
    co_return;
  });

  io.run();
  EXPECT_TRUE(testPassed);
}

TEST(DnsForwarderTest, ExposeConfigurationOfForwarderAndComponents) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto forwarder = std::make_shared<DnsForwarder>(io.get_executor());

    // Initially empty
    auto initConfig = forwarder->GetConfiguration();
    EXPECT_TRUE(initConfig.Listeners.empty());
    EXPECT_TRUE(initConfig.Upstreams.empty());
    EXPECT_FALSE(initConfig.DefaultRoute.has_value());
    EXPECT_TRUE(initConfig.Routes.empty());

    auto errStart = co_await forwarder->Start();
    EXPECT_FALSE(errStart);

    // 1. Add listeners
    boost::asio::ip::udp::endpoint ep1(boost::asio::ip::address_v4::loopback(), 53051);
    boost::asio::ip::udp::endpoint ep2(boost::asio::ip::address_v4::loopback(), 53052);
    auto l1Res = co_await forwarder->AddListener(ep1);
    EXPECT_TRUE(l1Res.has_value());
    auto l2Res = co_await forwarder->AddListener(ep2);
    EXPECT_TRUE(l2Res.has_value());

    // 2. Add upstreams
    boost::asio::ip::udp::endpoint srv1(boost::asio::ip::make_address_v4("8.8.8.8"), 53);
    boost::asio::ip::udp::endpoint srv2(boost::asio::ip::make_address_v4("8.8.4.4"), 53);
    auto u1Res = co_await forwarder->AddUpstream({srv1, srv2});
    EXPECT_TRUE(u1Res.has_value());

    boost::asio::ip::udp::endpoint srv3(boost::asio::ip::make_address_v4("10.0.0.1"), 53);
    auto u2Res = co_await forwarder->AddUpstream({srv3});
    EXPECT_TRUE(u2Res.has_value());

    // Verify component getters
    auto u1 = u1Res.value().lock();
    EXPECT_TRUE(u1 != nullptr);
    if (u1) {
      EXPECT_EQ(u1->GetUpstreamServers().size(), 2u);
      EXPECT_EQ(u1->GetUpstreamServers()[0], srv1);
      EXPECT_EQ(u1->GetUpstreamServers()[1], srv2);
    }

    // 3. Add routes
    forwarder->SetDefaultRoute(u1Res.value());
    forwarder->AddRoute("company.com", u2Res.value());
    forwarder->AddRoute("internal.company.com", u2Res.value());

    // 4. Query configuration
    auto config = forwarder->GetConfiguration();

    // Check listeners
    EXPECT_EQ(config.Listeners.size(), 2u);
    bool foundEp1 = false;
    bool foundEp2 = false;
    for (const auto& l : config.Listeners) {
      if (l.LocalEndpoint.Port == 53051) {
        foundEp1 = true;
        EXPECT_EQ(l.Listener.lock(), l1Res.value().lock());
        EXPECT_EQ(std::get<Interface::Ip4Address>(l.LocalEndpoint.Address).Bytes,
                  (std::array<uint8_t, 4>{127, 0, 0, 1}));
      } else if (l.LocalEndpoint.Port == 53052) {
        foundEp2 = true;
        EXPECT_EQ(l.Listener.lock(), l2Res.value().lock());
        EXPECT_EQ(std::get<Interface::Ip4Address>(l.LocalEndpoint.Address).Bytes,
                  (std::array<uint8_t, 4>{127, 0, 0, 1}));
      }
    }
    EXPECT_TRUE(foundEp1);
    EXPECT_TRUE(foundEp2);

    // Check upstreams and server endpoints
    EXPECT_EQ(config.Upstreams.size(), 2u);
    bool foundU1 = false;
    bool foundU2 = false;
    for (const auto& u : config.Upstreams) {
      EXPECT_GT(u.LocalPort, 0u);
      if (u.Upstream.lock() == u1Res.value().lock()) {
        foundU1 = true;
        EXPECT_EQ(u.ServerEndpoints.size(), 2u);
        if (u.ServerEndpoints.size() == 2u) {
          EXPECT_EQ(u.ServerEndpoints[0].Port, 53u);
          EXPECT_EQ(std::get<Interface::Ip4Address>(u.ServerEndpoints[0].Address).Bytes,
                    (std::array<uint8_t, 4>{8, 8, 8, 8}));
          EXPECT_EQ(u.ServerEndpoints[1].Port, 53u);
          EXPECT_EQ(std::get<Interface::Ip4Address>(u.ServerEndpoints[1].Address).Bytes,
                    (std::array<uint8_t, 4>{8, 8, 4, 4}));
        }
      } else if (u.Upstream.lock() == u2Res.value().lock()) {
        foundU2 = true;
        EXPECT_EQ(u.ServerEndpoints.size(), 1u);
        if (u.ServerEndpoints.size() == 1u) {
          EXPECT_EQ(u.ServerEndpoints[0].Port, 53u);
          EXPECT_EQ(std::get<Interface::Ip4Address>(u.ServerEndpoints[0].Address).Bytes,
                    (std::array<uint8_t, 4>{10, 0, 0, 1}));
        }
      }
    }
    EXPECT_TRUE(foundU1);
    EXPECT_TRUE(foundU2);

    // Check default route
    EXPECT_TRUE(config.DefaultRoute.has_value());
    if (config.DefaultRoute.has_value()) {
      EXPECT_EQ(config.DefaultRoute->lock(), u1Res.value().lock());
    }

    // Check routes
    EXPECT_EQ(config.Routes.size(), 2u);
    EXPECT_EQ(config.Routes.at("company.com").lock(), u2Res.value().lock());
    EXPECT_EQ(config.Routes.at("internal.company.com").lock(), u2Res.value().lock());

    // 5. Test dynamic reconfiguration reflected in configuration
    co_await forwarder->RemoveListener(l1Res.value());
    forwarder->RemoveRoute("internal.company.com");
    co_await forwarder->RemoveUpstream(u2Res.value());

    auto updatedConfig = forwarder->GetConfiguration();
    EXPECT_EQ(updatedConfig.Listeners.size(), 1u);
    EXPECT_EQ(updatedConfig.Listeners[0].LocalEndpoint.Port, 53052u);
    EXPECT_EQ(updatedConfig.Upstreams.size(), 1u);
    EXPECT_EQ(updatedConfig.Upstreams[0].Upstream.lock(), u1Res.value().lock());
    EXPECT_EQ(updatedConfig.Routes.size(), 1u);
    EXPECT_FALSE(updatedConfig.Routes.contains("internal.company.com"));
    // Upstream u2 was removed, so its weak_ptr in Routes["company.com"] is expired
    EXPECT_TRUE(updatedConfig.Routes.at("company.com").expired());

    co_await forwarder->Stop();
    testPassed = true;
    co_return;
  });

  io.run();
  EXPECT_TRUE(testPassed);
}
