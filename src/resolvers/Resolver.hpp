#pragma once

#include <boost/asio.hpp>
#include <boost/log/trivial.hpp>
#include <cstdint>
#include <expected>

#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "ServiceBase.hpp"

namespace gh {

struct SrvResult {
  std::string Target;
  uint16_t Port;
};

class ResolveFor {
public:
  enum class Protocol : uint8_t { Unspecified, Tcp, Udp };

  explicit ResolveFor() = default;
  virtual ~ResolveFor() = default;

  ResolveFor(const ResolveFor&) = default;
  auto operator=(const ResolveFor&) -> ResolveFor& = default;
  ResolveFor(ResolveFor&&) = delete;
  auto operator=(ResolveFor&&) -> ResolveFor& = delete;

  virtual auto GetExecutor() -> boost::asio::any_io_executor = 0;
  virtual auto GetService() -> std::string = 0;
  virtual auto GetProtocol() -> Protocol = 0;
};

class Resolver : public ServiceBase {
public:
  explicit Resolver() = default;
  virtual ~Resolver() override = default;

  Resolver(const Resolver&) = delete;
  auto operator=(const Resolver&) -> Resolver& = delete;
  Resolver(Resolver&&) = delete;
  auto operator=(Resolver&&) -> Resolver& = delete;

  template <typename Self>
  auto Resolve(this Self& self, Cancel& cancel)
      -> Omni::Fiber::Coroutine<std::expected<typename Self::ResultType, ErrorCode>> {
    BOOST_LOG_TRIVIAL(trace) << self.GetName() << " resolving...";
    if (auto err = co_await self.DoResolve(cancel)) {
      BOOST_LOG_TRIVIAL(trace) << self.GetName() << " resolution failed: " << err.message();
      co_return std::unexpected(err);
    } else {
      auto result = self.GetResolverResult();
      BOOST_LOG_TRIVIAL(trace) << self.GetName() << " resolved " << result;
      co_return result;
    }
  }

protected:
  auto DoResolve(Cancel& cancel) -> Omni::Fiber::Coroutine<ErrorCode>;

  ErrorCode _ResolveError;
};

// ==================== ResolverIp ====================
class ResolverIp : public Resolver {
public:
  explicit ResolverIp() = default;
  virtual ~ResolverIp() override = default;

  ResolverIp(const ResolverIp&) = delete;
  auto operator=(const ResolverIp&) -> ResolverIp& = delete;
  ResolverIp(ResolverIp&&) = delete;
  auto operator=(ResolverIp&&) -> ResolverIp& = delete;

  using ResultType = boost::asio::ip::address_v6;
  virtual auto GetResolverResult() const -> ResultType = 0;
};

// ==================== ResolverPort ====================
class ResolverPort : public Resolver {
public:
  explicit ResolverPort() = default;
  virtual ~ResolverPort() override = default;

  ResolverPort(const ResolverPort&) = delete;
  auto operator=(const ResolverPort&) -> ResolverPort& = delete;
  ResolverPort(ResolverPort&&) = delete;
  auto operator=(ResolverPort&&) -> ResolverPort& = delete;

  using ResultType = uint16_t;
  virtual auto GetResolverResult() const -> ResultType = 0;
};

// ==================== ResolverEndpoint ====================
class ResolverEndpoint : public Resolver {
public:
  explicit ResolverEndpoint() = default;
  virtual ~ResolverEndpoint() override = default;

  ResolverEndpoint(const ResolverEndpoint&) = delete;
  auto operator=(const ResolverEndpoint&) -> ResolverEndpoint& = delete;
  ResolverEndpoint(ResolverEndpoint&&) = delete;
  auto operator=(ResolverEndpoint&&) -> ResolverEndpoint& = delete;

  using ResultType = boost::asio::ip::udp::endpoint;
  virtual auto GetResolverResult() const -> ResultType = 0;
};

} // namespace gh
