#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "InterfaceCommonTypes.hpp"
#include "RemoteCall.hpp"
#include "ServiceBase.hpp"

namespace gh::dns {

class DnsUpstream;
class DnsListener;

class DnsRouter : public ServiceBase {
public:
  struct Configuration {
    std::optional<std::weak_ptr<DnsUpstream>> DefaultRoute;
    std::unordered_map<std::string, std::weak_ptr<DnsUpstream>> Routes;
  };

  explicit DnsRouter(Configuration config, Interface::DnsForwarderCallbacks& callbacks);
  ~DnsRouter() override;

  DnsRouter(const DnsRouter&) = delete;
  auto operator=(const DnsRouter&) -> DnsRouter& = delete;
  DnsRouter(DnsRouter&&) = delete;
  auto operator=(DnsRouter&&) -> DnsRouter& = delete;

  [[nodiscard]] auto GetName() const -> std::string override { return "DnsRouter"; }
  [[nodiscard]] auto GetRoutes() const -> const auto& { return _Routes; }
  [[nodiscard]] auto GetDefaultRoute() const -> const auto& { return _DefaultRoute; }

  [[nodiscard]] auto Route(const std::string& domain) const -> std::optional<std::shared_ptr<DnsUpstream>>;
  auto HandleRequest(DnsListener& listener, boost::asio::ip::udp::endpoint sender, std::vector<uint8_t> data)
      -> Omni::Fiber::Coroutine<void>;

protected:
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  struct RequestContext {
    std::shared_ptr<Omni::Fiber::Fiber> FiberHandle;
    std::shared_ptr<Cancel> CancelToken;
  };

  std::unordered_map<std::string, std::weak_ptr<DnsUpstream>> _Routes;
  std::optional<std::weak_ptr<DnsUpstream>> _DefaultRoute;

  Interface::DnsForwarderCallbacks& _Callbacks;

  Omni::Fiber::RemoteCall _Rpc;
  std::vector<RequestContext> _RequestFibers;
  uint64_t _NextRequestId{0};

  static auto NormalizeDomain(const std::string& domain) -> std::string;
};

} // namespace gh::dns
