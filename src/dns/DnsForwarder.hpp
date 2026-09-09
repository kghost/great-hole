#pragma once

#include <memory>
#include <set>
#include <vector>

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "DnsListener.hpp"
#include "DnsRouter.hpp"
#include "DnsUpstream.hpp"
#include "ErrorCode.hpp"
#include "InterfaceCommonTypes.hpp"
#include "RemoteCall.hpp"
#include "ServiceBase.hpp"

namespace gh::dns {

class DnsForwarder : public ServiceBase {
public:
  using Configuration = Interface::DnsForwarderConfiguration;

  explicit DnsForwarder(boost::asio::any_io_executor executor);
  ~DnsForwarder() override;

  DnsForwarder(const DnsForwarder&) = delete;
  auto operator=(const DnsForwarder&) -> DnsForwarder& = delete;
  DnsForwarder(DnsForwarder&&) = delete;
  auto operator=(DnsForwarder&&) -> DnsForwarder& = delete;

  [[nodiscard]] auto GetName() const -> std::string override { return "DnsForwarder"; }

  auto AddListener(boost::asio::ip::udp::endpoint endpoint)
      -> Omni::Fiber::Coroutine<std::expected<std::weak_ptr<DnsListener>, ErrorCode>>;
  auto RemoveListener(const std::weak_ptr<DnsListener>& weak) -> Omni::Fiber::Coroutine<void>;

  auto AddUpstream(std::vector<boost::asio::ip::udp::endpoint> upstreamServers)
      -> Omni::Fiber::Coroutine<std::expected<std::weak_ptr<DnsUpstream>, ErrorCode>>;
  auto RemoveUpstream(const std::weak_ptr<DnsUpstream>& weak) -> Omni::Fiber::Coroutine<void>;

  void AddRoute(const std::string& domainSuffix, std::weak_ptr<DnsUpstream> upstream);
  void RemoveRoute(const std::string& domainSuffix);
  void SetDefaultRoute(std::weak_ptr<DnsUpstream> upstream);

  [[nodiscard]] auto GetConfiguration() const -> Configuration;

protected:
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  boost::asio::any_io_executor _Executor;

  std::shared_ptr<DnsRouter> _Router;
  std::set<std::shared_ptr<DnsListener>, std::owner_less<>> _Listeners;
  std::set<std::shared_ptr<DnsUpstream>, std::owner_less<>> _Upstreams;
  Omni::Fiber::RemoteCall _ClientRpc;
};

} // namespace gh::dns
