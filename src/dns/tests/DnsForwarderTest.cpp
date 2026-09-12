#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "Cancel.hpp"
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
  boost::asio::io_context io;

  auto defaultClient =
      std::make_shared<DnsUpstream>(io.get_executor(), "default", std::vector<boost::asio::ip::udp::endpoint>{});
  auto companyClient =
      std::make_shared<DnsUpstream>(io.get_executor(), "company", std::vector<boost::asio::ip::udp::endpoint>{});
  auto subCompanyClient =
      std::make_shared<DnsUpstream>(io.get_executor(), "subCompany", std::vector<boost::asio::ip::udp::endpoint>{});

  DnsRouter::Configuration config{
      .DefaultRoute = defaultClient,
      .Routes =
          {
              {"company.com", companyClient},
              {"internal.company.com", subCompanyClient},
          },
  };
  auto router = std::make_shared<DnsRouter>(std::move(config));

  // Exact & Suffix matching
  EXPECT_EQ(router->Route("foo.bar.org"), defaultClient);
  EXPECT_EQ(router->Route("company.com"), companyClient);
  EXPECT_EQ(router->Route("web.company.com"), companyClient);
  EXPECT_EQ(router->Route("internal.company.com"), subCompanyClient);
  EXPECT_EQ(router->Route("api.internal.company.com"), subCompanyClient);

  // Test weak_ptr expiration handling
  subCompanyClient.reset();
  EXPECT_EQ(router->Route("api.internal.company.com"), companyClient);

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
    auto client =
        std::make_shared<DnsUpstream>(io.get_executor(), "test_upstream",
                                      std::vector<boost::asio::ip::udp::endpoint>{boost::asio::ip::udp::endpoint(
                                          boost::asio::ip::address_v4::loopback(), 53053)});

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

namespace {

auto ToDnsEndpoint(const boost::asio::ip::udp::endpoint& ep) -> Interface::DnsEndpoint {
  if (ep.address().is_v4()) {
    return Interface::DnsEndpoint{
        .Address = Interface::Ip4Address{.Bytes = ep.address().to_v4().to_bytes()},
        .Port = ep.port(),
    };
  }
  return Interface::DnsEndpoint{
      .Address = Interface::Ip6Address{.Bytes = ep.address().to_v6().to_bytes()},
      .Port = ep.port(),
  };
}

} // namespace

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

    Interface::DnsForwarderConfiguration config{
        .Listeners = {{.LocalEndpoint = ToDnsEndpoint(forwarderBindEp)}},
        .Upstreams = {{.Name = "mock", .ServerEndpoints = {ToDnsEndpoint(mockUpstreamEp)}}},
        .DefaultRoute = "mock",
    };

    auto forwarder = std::make_shared<DnsForwarder>(io.get_executor(), std::move(config));
    auto errStart = co_await forwarder->Start();
    EXPECT_FALSE(errStart);

    boost::asio::ip::udp::endpoint forwarderListenEp = forwarderBindEp;

    // Send test query to DnsForwarder listener
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

    // Receive response from DnsForwarder
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

TEST(DnsForwarderTest, ConstructWithMultipleUpstreamsAndDomainRouting) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    // 1. Mock Default Upstream Server (returns 1.1.1.1)
    boost::asio::ip::udp::socket defaultMockSock(io.get_executor());
    defaultMockSock.open(boost::asio::ip::udp::v4());
    defaultMockSock.bind(boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    boost::asio::ip::udp::endpoint defaultMockEp = defaultMockSock.local_endpoint();

    auto defaultFiber = (co_await Omni::Fiber::GetCurrentOmniFiber())
                            .Spawn("DefaultMock", [&defaultMockSock]() -> Omni::Fiber::Coroutine<void> {
                              std::vector<uint8_t> buf(2048);
                              boost::asio::ip::udp::endpoint clientEp;
                              auto [ec, n] = co_await defaultMockSock.async_receive_from(
                                  boost::asio::buffer(buf), clientEp, Omni::Fiber::AsioUseFiber);
                              if (!ec) {
                                auto req = DnsPacket::Parse(std::span<const uint8_t>(buf.data(), n));
                                if (req) {
                                  DnsPacket resp;
                                  resp.Header.Id = req->Header.Id;
                                  resp.Header.SetResponse(true);
                                  resp.Questions = req->Questions;
                                  resp.Answers.push_back(DnsResourceRecord{
                                      .Name = req->GetPrimaryQName(),
                                      .Type = static_cast<uint16_t>(DnsType::A),
                                      .Ttl = 60,
                                      .RData = {1, 1, 1, 1},
                                  });
                                  auto wire = resp.Serialize();
                                  co_await defaultMockSock.async_send_to(boost::asio::buffer(wire), clientEp,
                                                                         Omni::Fiber::AsioUseFiber);
                                }
                              }
                            });

    // 2. Mock Corp Upstream Server (returns 10.0.0.1)
    boost::asio::ip::udp::socket corpMockSock(io.get_executor());
    corpMockSock.open(boost::asio::ip::udp::v4());
    corpMockSock.bind(boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    boost::asio::ip::udp::endpoint corpMockEp = corpMockSock.local_endpoint();

    auto corpFiber =
        (co_await Omni::Fiber::GetCurrentOmniFiber())
            .Spawn("CorpMock", [&corpMockSock]() -> Omni::Fiber::Coroutine<void> {
              std::vector<uint8_t> buf(2048);
              boost::asio::ip::udp::endpoint clientEp;
              auto [ec, n] = co_await corpMockSock.async_receive_from(boost::asio::buffer(buf), clientEp,
                                                                      Omni::Fiber::AsioUseFiber);
              if (!ec) {
                auto req = DnsPacket::Parse(std::span<const uint8_t>(buf.data(), n));
                if (req) {
                  DnsPacket resp;
                  resp.Header.Id = req->Header.Id;
                  resp.Header.SetResponse(true);
                  resp.Questions = req->Questions;
                  resp.Answers.push_back(DnsResourceRecord{
                      .Name = req->GetPrimaryQName(),
                      .Type = static_cast<uint16_t>(DnsType::A),
                      .Ttl = 60,
                      .RData = {10, 0, 0, 1},
                  });
                  auto wire = resp.Serialize();
                  co_await corpMockSock.async_send_to(boost::asio::buffer(wire), clientEp, Omni::Fiber::AsioUseFiber);
                }
              }
            });

    // 3. Configure DnsForwarder with routes
    boost::asio::ip::udp::endpoint listenEp(boost::asio::ip::address_v4::loopback(), 0);
    boost::asio::ip::udp::socket tempSock(io.get_executor());
    tempSock.open(listenEp.protocol());
    tempSock.bind(listenEp);
    boost::asio::ip::udp::endpoint forwarderBindEp = tempSock.local_endpoint();
    tempSock.close();

    Interface::DnsForwarderConfiguration config{
        .Listeners = {{.LocalEndpoint = ToDnsEndpoint(forwarderBindEp)}},
        .Upstreams =
            {
                {.Name = "default_upstream", .ServerEndpoints = {ToDnsEndpoint(defaultMockEp)}},
                {.Name = "corp_upstream", .ServerEndpoints = {ToDnsEndpoint(corpMockEp)}},
            },
        .DefaultRoute = "default_upstream",
        .Routes = {{"internal.company.com", "corp_upstream"}},
    };

    auto forwarder = std::make_shared<DnsForwarder>(io.get_executor(), std::move(config));
    auto errStart = co_await forwarder->Start();
    EXPECT_FALSE(errStart);

    boost::asio::ip::udp::socket clientSocket(io.get_executor());
    clientSocket.open(boost::asio::ip::udp::v4());
    clientSocket.bind(boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::loopback(), 0));

    // Send query for "internal.company.com" -> expect 10.0.0.1 from corp_upstream
    {
      DnsPacket queryPacket;
      queryPacket.Header.Id = 0x1111;
      queryPacket.Questions.push_back(
          DnsQuestion{.QName = "api.internal.company.com", .QType = static_cast<uint16_t>(DnsType::A)});
      auto wire = queryPacket.Serialize();
      co_await clientSocket.async_send_to(boost::asio::buffer(wire), forwarderBindEp, Omni::Fiber::AsioUseFiber);

      std::vector<uint8_t> respBuf(2048);
      boost::asio::ip::udp::endpoint senderEp;
      auto [ec, n] =
          co_await clientSocket.async_receive_from(boost::asio::buffer(respBuf), senderEp, Omni::Fiber::AsioUseFiber);
      EXPECT_FALSE(ec);
      auto resp = DnsPacket::Parse(std::span<const uint8_t>(respBuf.data(), n));
      EXPECT_TRUE(resp.has_value());
      if (resp.has_value()) {
        EXPECT_EQ(resp->Answers.size(), 1u);
        if (!resp->Answers.empty()) {
          EXPECT_EQ(resp->Answers[0].RData, (std::vector<uint8_t>{10, 0, 0, 1}));
        }
      }
    }

    // Cleanup
    defaultMockSock.close();
    corpMockSock.close();
    co_await (co_await Omni::Fiber::GetCurrentOmniFiber()).Join(defaultFiber);
    co_await (co_await Omni::Fiber::GetCurrentOmniFiber()).Join(corpFiber);
    clientSocket.close();
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
    boost::asio::ip::udp::endpoint ep1(boost::asio::ip::address_v4::loopback(), 53051);
    boost::asio::ip::udp::endpoint ep2(boost::asio::ip::address_v4::loopback(), 53052);
    boost::asio::ip::udp::endpoint srv1(boost::asio::ip::make_address_v4("8.8.8.8"), 53);
    boost::asio::ip::udp::endpoint srv2(boost::asio::ip::make_address_v4("8.8.4.4"), 53);
    boost::asio::ip::udp::endpoint srv3(boost::asio::ip::make_address_v4("10.0.0.1"), 53);

    Interface::DnsForwarderConfiguration initConfig{
        .Listeners = {{.LocalEndpoint = ToDnsEndpoint(ep1)}, {.LocalEndpoint = ToDnsEndpoint(ep2)}},
        .Upstreams =
            {
                {.Name = "u1", .ServerEndpoints = {ToDnsEndpoint(srv1), ToDnsEndpoint(srv2)}},
                {.Name = "u2", .ServerEndpoints = {ToDnsEndpoint(srv3)}},
            },
        .DefaultRoute = "u1",
        .Routes = {{"company.com", "u2"}, {"internal.company.com", "u2"}},
    };

    auto forwarder = std::make_shared<DnsForwarder>(io.get_executor(), std::move(initConfig));

    auto errStart = co_await forwarder->Start();
    EXPECT_FALSE(errStart);

    // Query configuration after starting
    auto config = forwarder->GetConfiguration();

    // Check listeners
    EXPECT_EQ(config.Listeners.size(), 2u);
    bool foundEp1 = false;
    bool foundEp2 = false;
    for (const auto& l : config.Listeners) {
      if (l.LocalEndpoint.Port == 53051) {
        foundEp1 = true;
        EXPECT_EQ(std::get<Interface::Ip4Address>(l.LocalEndpoint.Address).Bytes,
                  (std::array<uint8_t, 4>{127, 0, 0, 1}));
      } else if (l.LocalEndpoint.Port == 53052) {
        foundEp2 = true;
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
      if (u.Name == "u1") {
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
      } else if (u.Name == "u2") {
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
      EXPECT_EQ(*config.DefaultRoute, "u1");
    }

    // Check routes
    EXPECT_EQ(config.Routes.size(), 2u);
    EXPECT_EQ(config.Routes.at("company.com"), "u2");
    EXPECT_EQ(config.Routes.at("internal.company.com"), "u2");

    co_await forwarder->Stop();
    testPassed = true;
    co_return;
  });

  io.run();
  EXPECT_TRUE(testPassed);
}

TEST(DnsForwarderIntegrationTest, HandleMultipleSequentialQueries) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    boost::asio::ip::udp::endpoint mockUpstreamBindEp(boost::asio::ip::address_v4::loopback(), 0);
    auto mockSocket = std::make_shared<boost::asio::ip::udp::socket>(io.get_executor());
    mockSocket->open(mockUpstreamBindEp.protocol());
    mockSocket->bind(mockUpstreamBindEp);
    boost::asio::ip::udp::endpoint mockUpstreamEp = mockSocket->local_endpoint();

    Cancel stopMock;
    auto mockUpstreamFiber =
        (co_await Omni::Fiber::GetCurrentOmniFiber())
            .Spawn("MockUpstream", [mockSocket, &stopMock]() -> Omni::Fiber::Coroutine<void> {
              while (!stopMock.IsTriggered()) {
                std::vector<uint8_t> buf(2048);
                boost::asio::ip::udp::endpoint clientEp;

                auto [ec, n] =
                    co_await mockSocket->async_receive_from(boost::asio::buffer(buf), clientEp, stopMock.AsioSlot()());
                if (ec) {
                  break;
                }

                auto req = DnsPacket::Parse(std::span<const uint8_t>(buf.data(), n));
                if (req) {
                  DnsPacket resp;
                  resp.Header.Id = req->Header.Id;
                  resp.Header.SetResponse(true);
                  resp.Questions = req->Questions;

                  DnsResourceRecord rr;
                  rr.Name = req->GetPrimaryQName();
                  rr.Type = static_cast<uint16_t>(DnsType::A);
                  rr.Ttl = 60;
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

    Interface::DnsForwarderConfiguration config{
        .Listeners = {{.LocalEndpoint = ToDnsEndpoint(forwarderBindEp)}},
        .Upstreams = {{.Name = "mock", .ServerEndpoints = {ToDnsEndpoint(mockUpstreamEp)}}},
        .DefaultRoute = "mock",
    };

    auto forwarder = std::make_shared<DnsForwarder>(io.get_executor(), std::move(config));
    auto errStart = co_await forwarder->Start();
    EXPECT_FALSE(errStart);

    boost::asio::ip::udp::socket testClientSocket(io.get_executor());
    testClientSocket.open(boost::asio::ip::udp::v4());
    testClientSocket.bind(boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::loopback(), 0));

    // Send and verify multiple sequential queries
    for (uint16_t i = 1; i <= 5; ++i) {
      DnsPacket queryPacket;
      queryPacket.Header.Id = 0x1000 + i;
      DnsQuestion q;
      q.QName = "query" + std::to_string(i) + ".example.com";
      q.QType = static_cast<uint16_t>(DnsType::A);
      queryPacket.Questions.push_back(q);

      auto queryWire = queryPacket.Serialize();
      co_await testClientSocket.async_send_to(boost::asio::buffer(queryWire), forwarderBindEp,
                                              Omni::Fiber::AsioUseFiber);

      std::vector<uint8_t> respBuf(2048);
      boost::asio::ip::udp::endpoint senderEp;
      auto [ecRecv, respLen] = co_await testClientSocket.async_receive_from(boost::asio::buffer(respBuf), senderEp,
                                                                            Omni::Fiber::AsioUseFiber);
      EXPECT_FALSE(ecRecv);
      auto respPacket = DnsPacket::Parse(std::span<const uint8_t>(respBuf.data(), respLen));
      EXPECT_TRUE(respPacket.has_value());
      if (respPacket.has_value()) {
        EXPECT_EQ(respPacket->Header.Id, 0x1000 + i);
        EXPECT_TRUE(respPacket->Header.IsResponse());
        EXPECT_EQ(respPacket->Answers.size(), 1u);
        if (!respPacket->Answers.empty()) {
          EXPECT_EQ(respPacket->Answers[0].Name, "query" + std::to_string(i) + ".example.com");
          EXPECT_EQ(respPacket->Answers[0].RData, (std::vector<uint8_t>{192, 168, 1, 100}));
        }
      }
    }

    // Cleanup
    stopMock.Trigger();
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
