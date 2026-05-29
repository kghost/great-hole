#pragma once

#include <memory>
#include <string>
#include <vector>

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "Event.hpp"
#include "ServiceBase.hpp"

namespace gh {

// ==================== ResolverIp ====================
class ResolverIp : public ServiceBase {
public:
  virtual ~ResolverIp() override = default;

  virtual std::vector<boost::asio::ip::address> GetAddresses() const = 0;
};

// ==================== ResolverPort ====================
class ResolverPort : public ServiceBase {
public:
  virtual ~ResolverPort() override = default;

  virtual uint16_t GetPort() const = 0;
};

// ==================== ResolverStaticIp ====================
class ResolverStaticIp final : public ResolverIp {
public:
  explicit ResolverStaticIp(std::string const& ipStr);
  ~ResolverStaticIp() override = default;

  ResolverStaticIp(const ResolverStaticIp&) = delete;
  ResolverStaticIp& operator=(const ResolverStaticIp&) = delete;
  ResolverStaticIp(ResolverStaticIp&&) = delete;
  ResolverStaticIp& operator=(ResolverStaticIp&&) = delete;

  std::vector<boost::asio::ip::address> GetAddresses() const override;

protected:
  std::string GetName() const override { return "ResolverStaticIp"; }
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  std::string _IpStr;
  std::vector<boost::asio::ip::address> _Addresses;
};

// ==================== ResolverStaticDns ====================
class ResolverStaticDns final : public ResolverIp {
public:
  explicit ResolverStaticDns(boost::asio::io_context& ioContext, std::string const& host);
  ~ResolverStaticDns() override = default;

  ResolverStaticDns(const ResolverStaticDns&) = delete;
  ResolverStaticDns& operator=(const ResolverStaticDns&) = delete;
  ResolverStaticDns(ResolverStaticDns&&) = delete;
  ResolverStaticDns& operator=(ResolverStaticDns&&) = delete;

  std::vector<boost::asio::ip::address> GetAddresses() const override;

protected:
  std::string GetName() const override { return "ResolverStaticDns"; }
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  boost::asio::io_context& _IoContext;
  std::string _Host;
  std::vector<boost::asio::ip::address> _Addresses;
};

// ==================== ResolverStaticPort ====================
class ResolverStaticPort final : public ResolverPort {
public:
  explicit ResolverStaticPort(std::string const& portStr);
  explicit ResolverStaticPort(uint16_t port);
  ~ResolverStaticPort() override = default;

  ResolverStaticPort(const ResolverStaticPort&) = delete;
  ResolverStaticPort& operator=(const ResolverStaticPort&) = delete;
  ResolverStaticPort(ResolverStaticPort&&) = delete;
  ResolverStaticPort& operator=(ResolverStaticPort&&) = delete;

  uint16_t GetPort() const override;

protected:
  std::string GetName() const override { return "ResolverStaticPort"; }
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  std::string _PortStr;
  uint16_t _Port = 0;
  bool _IsNumeric = false;
};

// ==================== ResolverEndpoint ====================
class ResolverEndpoint : public ServiceBase {
public:
  virtual ~ResolverEndpoint() override = default;

  virtual std::vector<boost::asio::ip::udp::endpoint> GetEndpoints() const = 0;
};

// ==================== ResolverCombinedEndpoint ====================
class ResolverCombinedEndpoint final : public ResolverEndpoint {
public:
  explicit ResolverCombinedEndpoint(boost::asio::io_context& ioContext,
                                    std::shared_ptr<ResolverIp> ipResolver,
                                    std::shared_ptr<ResolverPort> portResolver);
  ~ResolverCombinedEndpoint() override = default;

  ResolverCombinedEndpoint(const ResolverCombinedEndpoint&) = delete;
  ResolverCombinedEndpoint& operator=(const ResolverCombinedEndpoint&) = delete;
  ResolverCombinedEndpoint(ResolverCombinedEndpoint&&) = delete;
  ResolverCombinedEndpoint& operator=(ResolverCombinedEndpoint&&) = delete;

  std::vector<boost::asio::ip::udp::endpoint> GetEndpoints() const override;

protected:
  std::string GetName() const override { return "ResolverCombinedEndpoint"; }
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  boost::asio::io_context& _IoContext;
  std::shared_ptr<ResolverIp> _IpResolver;
  std::shared_ptr<ResolverPort> _PortResolver;
  std::vector<boost::asio::ip::udp::endpoint> _Endpoints;
};

// ==================== ResolverDnsService ====================
class ResolverDnsService final : public ResolverEndpoint {
public:
  explicit ResolverDnsService(boost::asio::io_context& ioContext, std::string const& serviceName);
  ~ResolverDnsService() override = default;

  ResolverDnsService(const ResolverDnsService&) = delete;
  ResolverDnsService& operator=(const ResolverDnsService&) = delete;
  ResolverDnsService(ResolverDnsService&&) = delete;
  ResolverDnsService& operator=(ResolverDnsService&&) = delete;

  std::vector<boost::asio::ip::udp::endpoint> GetEndpoints() const override;

protected:
  std::string GetName() const override { return "ResolverDnsService"; }
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  boost::asio::io_context& _IoContext;
  std::string _ServiceName;
  std::vector<boost::asio::ip::udp::endpoint> _Endpoints;
};

} // namespace gh
