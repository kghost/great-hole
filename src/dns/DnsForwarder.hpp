#pragma once

#include <memory>
#include <string>
#include <vector>

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "DnsListener.hpp"
#include "DnsRouter.hpp"
#include "DnsUpstream.hpp"
#include "ErrorCode.hpp"
#include "InterfaceCommonTypes.hpp"
#include "ServiceBase.hpp"

namespace gh::dns {

class DnsForwarder : public ServiceBase {
public:
  using Configuration = Interface::DnsForwarderConfiguration;

  explicit DnsForwarder(boost::asio::any_io_executor executor, Configuration config);
  ~DnsForwarder() override;

  DnsForwarder(const DnsForwarder&) = delete;
  auto operator=(const DnsForwarder&) -> DnsForwarder& = delete;
  DnsForwarder(DnsForwarder&&) = delete;
  auto operator=(DnsForwarder&&) -> DnsForwarder& = delete;

  [[nodiscard]] auto GetName() const -> std::string override { return "DnsForwarder"; }

  [[nodiscard]] auto GetConfiguration() const -> Configuration;

protected:
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  boost::asio::any_io_executor _Executor;

  std::shared_ptr<DnsRouter> _Router;
  std::vector<std::shared_ptr<DnsListener>> _Listeners;
  std::vector<std::shared_ptr<DnsUpstream>> _Upstreams;
};

} // namespace gh::dns
