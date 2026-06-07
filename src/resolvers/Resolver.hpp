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
  virtual boost::asio::any_io_executor GetExecutor() = 0;
  virtual std::string GetService() = 0;
  virtual Protocol GetProtocol() = 0;
};

class ResolverBase : public ServiceBase {
public:
  virtual ~ResolverBase() override = default;

protected:
  Omni::Fiber::Coroutine<ErrorCode> DoResolve();
};

template <typename ResultType> class Resolver : public ResolverBase {
public:
  virtual ~Resolver() override = default;

  virtual ResultType GetResolverResult() const = 0;

  Omni::Fiber::Coroutine<std::expected<ResultType, ErrorCode>> Resolve() {
    if (auto err = co_await DoResolve()) {
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
