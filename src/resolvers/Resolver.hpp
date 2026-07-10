#pragma once

#include <boost/asio.hpp>
#include <expected>

#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "ServiceBase.hpp"

namespace gh {

class ResolveFor {
public:
  enum class Protocol { Unspecified, Tcp, Udp };

  virtual ~ResolveFor() = default;
  virtual auto GetExecutor() -> boost::asio::any_io_executor = 0;
  virtual auto GetService() -> std::string = 0;
  virtual auto GetProtocol() -> Protocol = 0;
};

class ResolverBase : public ServiceBase {
public:
  virtual ~ResolverBase() override = default;

protected:
  auto DoResolve(Cancel& c) -> Omni::Fiber::Coroutine<ErrorCode>;

  ErrorCode _ResolveError;
};

template <typename ResultType> class Resolver : public ResolverBase {
public:
  virtual ~Resolver() override = default;

  virtual auto GetResolverResult() const -> ResultType = 0;

  auto Resolve(Cancel& c) -> Omni::Fiber::Coroutine<std::expected<ResultType, ErrorCode>> {
    if (auto err = co_await DoResolve(c)) {
      co_return std::unexpected(err);
    }
    co_return GetResolverResult();
  }
};

// ==================== ResolverIp ====================
class ResolverIp : public Resolver<boost::asio::ip::address_v6> {
public:
  virtual ~ResolverIp() override = default;
};

// ==================== ResolverPort ====================
class ResolverPort : public Resolver<uint16_t> {
public:
  virtual ~ResolverPort() override = default;
};

// ==================== ResolverEndpoint ====================
class ResolverEndpoint : public Resolver<boost::asio::ip::udp::endpoint> {
public:
  virtual ~ResolverEndpoint() override = default;
};

} // namespace gh
