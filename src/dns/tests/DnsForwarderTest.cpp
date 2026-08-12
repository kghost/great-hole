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
