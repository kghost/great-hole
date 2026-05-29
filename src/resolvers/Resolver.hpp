#pragma once

#include <boost/asio.hpp>

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

// ==================== ResolverIp ====================
class ResolverIp : public ServiceBase {
public:
  virtual ~ResolverIp() override = default;

  virtual boost::asio::ip::address GetAddress() const = 0;
};

// ==================== ResolverPort ====================
class ResolverPort : public ServiceBase {
public:
  virtual ~ResolverPort() override = default;

  virtual uint16_t GetPort() const = 0;
};

// ==================== ResolverEndpoint ====================
class ResolverEndpoint : public ServiceBase {
public:
  virtual ~ResolverEndpoint() override = default;

  virtual boost::asio::ip::udp::endpoint GetEndpoint() const = 0;
};

} // namespace gh
