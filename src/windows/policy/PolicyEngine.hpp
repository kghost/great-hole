#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <string>

#include "DeferredPacketInjector.hpp"
#include "PolicyRegistry.hpp"
#include "PolicySelector.hpp"
#include "ServiceBase.hpp"
#include "WinDivertFlowSniffer.hpp"

namespace gh::policy {

class PolicyEngine : public ServiceBase {
public:
  explicit PolicyEngine(boost::asio::any_io_executor executor, gh::DeferredPacketInjector& injector);
  ~PolicyEngine() override;

  PolicyEngine(const PolicyEngine&) = delete;
  auto operator=(const PolicyEngine&) -> PolicyEngine& = delete;
  PolicyEngine(PolicyEngine&&) = delete;
  auto operator=(PolicyEngine&&) -> PolicyEngine& = delete;

  auto GetName() const -> std::string override { return "PolicyEngine"; }

  [[nodiscard]] auto GetPolicyRegistry() -> PolicyRegistry& { return _Registry; }
  [[nodiscard]] auto GetPolicySelector() -> PolicySelector& { return _Selector; }

  void ClearRegistry();
  void AddPathBypassRule(const std::string& path, PolicyScope scope);
  void AddPathEndpointRule(const std::string& path, const std::shared_ptr<gh::VpnClientMultiChannelSession>& endpoint,
                           PolicyScope scope);
  void RemovePathRule(const std::string& path);
  void AddPidEndpointRule(DWORD pid, const std::shared_ptr<gh::VpnClientMultiChannelSession>& endpoint,
                          PolicyScope scope);
  void SetDefaultEndpoint(const std::shared_ptr<gh::VpnClientMultiChannelSession>& endpoint);
  void SetDefaultBypass();
  auto LaunchWithPolicy(const std::string& commandLine,
                        const std::shared_ptr<gh::VpnClientMultiChannelSession>& endpoint, PolicyScope scope)
      -> uint32_t;

protected:
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  boost::asio::any_io_executor _Executor;
  PolicyRegistry _Registry;
  PolicySelector _Selector;
  std::shared_ptr<gh::WinDivertFlowSniffer> _Sniffer;
};

} // namespace gh::policy
