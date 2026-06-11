#include <memory>
#include <vector>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "EndpointTunSplitIp.hpp"
#include "Filter.hpp"
#include "GetCurrentFiber.hpp"
#include "VpnServer.hpp"

using namespace gh;

namespace {

class MockFilter : public Filter {
public:
  MockFilter(int& counter) : _Counter(counter) {}
  ~MockFilter() override = default;

  Omni::Fiber::Coroutine<boost::system::error_code> Pipe(Packet& /*p*/, Cancel& /*c*/) override {
    _Counter++;
    co_return boost::system::error_code{};
  }

private:
  int& _Counter;
};

} // namespace

TEST(VpnServerTest, ConstructorStoresFiltersAndAppliesThem) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io);
  Omni::Fiber::Manager manager(executor);

  int fds[2];
  ASSERT_GE(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds), 0);
  int testFd = fds[0];
  int externalFd = fds[1];

  auto tunSplit = std::make_shared<EndpointTunSplitIp>(io, "test", testFd);
  int filterCounter = 0;
  auto mockFilter = std::make_shared<MockFilter>(filterCounter);
  std::vector<std::shared_ptr<Filter>> filters = {mockFilter};

  auto vpnServer = std::make_shared<VpnServer>(tunSplit, filters);

  bool testPassed = false;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    auto tunErr = co_await tunSplit->Start();
    EXPECT_FALSE(tunErr);

    auto vpnErr = co_await vpnServer->Start();
    EXPECT_FALSE(vpnErr);

    auto vpnStopErr = co_await vpnServer->Stop();
    EXPECT_FALSE(vpnStopErr);

    co_await vpnServer->WaitService();

    co_await tunSplit->Stop();

    auto& current = co_await Omni::Fiber::GetCurrentFiber();
    co_await current.WaitAll();

    ::close(externalFd);
    testPassed = true;
    co_return;
  });

  io.restart();
  io.run();
  EXPECT_TRUE(testPassed);
}
