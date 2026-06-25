#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <boost/asio.hpp>

#include "ConnectionTracker.hpp"
#include "Coroutine.hpp"
#include "Endpoint.hpp"
#include "EndpointUdpDynMux.hpp"
#include "GHApi.hpp"
#include "VpnClientMultiChannel.hpp"

namespace gh {

enum class TunnelState { Starting = 0, Running = 1, Stopping = 2, Stopped = 3, Failed = 4 };

class GH_API DataPlaneCallbacks {
public:
  virtual ~DataPlaneCallbacks() = default;
  virtual void OnVpnStateChanged(TunnelState state, const std::string& message) = 0;
  virtual void OnTunnelStateChanged(int64_t endpointHandle, int state, const std::string& error) = 0;
};

class GH_API TunnelDataPlane : public VpnClientMultiChannel::SessionStateListener {
public:
  TunnelDataPlane(boost::asio::any_io_executor executor, ConnectionTracker::Selector& selector,
                  DataPlaneCallbacks& callbacks);
  ~TunnelDataPlane();

  void OnSessionStarting(VpnClientMultiChannel::Session& session) override;
  void OnSessionRunning(VpnClientMultiChannel::Session& session) override;
  void OnSessionStopping(VpnClientMultiChannel::Session& session) override;
  void OnSessionStopped(VpnClientMultiChannel::Session& session) override;
  void OnSessionFailed(VpnClientMultiChannel::Session& session, const std::string& error) override;

  TunnelDataPlane(const TunnelDataPlane&) = delete;
  TunnelDataPlane& operator=(const TunnelDataPlane&) = delete;
  TunnelDataPlane(TunnelDataPlane&&) = delete;
  TunnelDataPlane& operator=(TunnelDataPlane&&) = delete;

#if defined(_WIN32)
  Omni::Fiber::Coroutine<void> Start(int mtu, std::vector<char> encryptionKey);
#else
  Omni::Fiber::Coroutine<void> Start(int tunFd, int mtu, std::vector<char> encryptionKey);
  Omni::Fiber::Coroutine<void> MigrateTun(int tunFd);
#endif
  Omni::Fiber::Coroutine<void> Stop();
  Omni::Fiber::Coroutine<VpnClientMultiChannel::Session*> AddEndpoint(const UdpDynMux::PskType& psk,
                                                                      const std::string& host, int port);
  Omni::Fiber::Coroutine<void> RemoveEndpoint(VpnClientMultiChannel::Session* handle);

  std::optional<std::reference_wrapper<ConnectionMark>> FindSessionByHandle(VpnClientMultiChannel::Session* session);

  std::optional<VpnTrafficStats> GetTrafficStats(VpnClientMultiChannel::Session* session);

private:
  boost::asio::any_io_executor _Executor;
  ConnectionTracker::Selector& _Selector;
  DataPlaneCallbacks& _Callbacks;

  std::shared_ptr<Endpoint> _Tun;
  std::shared_ptr<UdpDynMux> _UdpDynMux;
  std::shared_ptr<VpnClientMultiChannel> _Client;

  std::set<VpnClientMultiChannel::Session*> _Endpoints;
  bool _Running = false;
};

} // namespace gh
