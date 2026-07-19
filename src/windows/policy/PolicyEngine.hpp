#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <string>

#include "PolicyRegistry.hpp"
#include "PolicySelector.hpp"
#include "ServiceBase.hpp"
#include "WinDivertFlowSniffer.hpp"

namespace gh::policy {

class PolicyEngine : public ServiceBase {
public:
  explicit PolicyEngine(boost::asio::any_io_executor executor);
  ~PolicyEngine() override;

  PolicyEngine(const PolicyEngine&) = delete;
  auto operator=(const PolicyEngine&) -> PolicyEngine& = delete;
  PolicyEngine(PolicyEngine&&) = delete;
  auto operator=(PolicyEngine&&) -> PolicyEngine& = delete;

  auto GetName() const -> std::string override { return "PolicyEngine"; }

  [[nodiscard]] auto GetPolicyRegistry() -> PolicyRegistry& { return _Registry; }
  [[nodiscard]] auto GetPolicySelector() -> PolicySelector& { return _Selector; }

  void ClearRegistry();
  void AddPathPolicy(const std::string& path, const PolicyRule& policy);
  void RemovePathPolicy(const std::string& path);
  void AddPidPolicy(DWORD pid, const PolicyRule& policy);
  void SetDefaultPolicy(const PolicyRule& policy);
  auto LaunchWithPolicy(const std::string& commandLine, const PolicyRule& policy) -> uint32_t;

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
