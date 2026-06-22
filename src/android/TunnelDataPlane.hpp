#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "EndpointTun.hpp"
#include "EndpointUdpDynMux.hpp"
#include "VpnClientMultiChannel.hpp"

namespace gh {

enum class TunnelState { Starting = 0, Running = 1, Stopping = 2, Stopped = 3, Failed = 4 };

class DataPlaneCallbacks {
public:
  virtual ~DataPlaneCallbacks() = default;
  virtual void OnVpnStateChanged(TunnelState state, const std::string& message) = 0;
  virtual void OnTunnelStateChanged(int64_t endpointHandle, int state, const std::string& error) = 0;
};

class TunnelDataPlane : public VpnClientMultiChannel::SessionStateListener {
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

  Omni::Fiber::Coroutine<void> Start(int tunFd, int mtu, std::vector<char> encryptionKey);
  Omni::Fiber::Coroutine<void> MigrateTun(int tunFd);
  Omni::Fiber::Coroutine<void> Stop();
  Omni::Fiber::Coroutine<VpnClientMultiChannel::Session*> AddEndpoint(const UdpDynMux::PskType& psk,
                                                                      const std::string& host, int port);
  Omni::Fiber::Coroutine<void> RemoveEndpoint(VpnClientMultiChannel::Session* handle);

  std::optional<std::reference_wrapper<ConnectionMark>> FindSessionByHandle(VpnClientMultiChannel::Session* session);

  std::optional<TrafficStats> GetTrafficStats(VpnClientMultiChannel::Session* session);

private:
  boost::asio::any_io_executor _Executor;
  ConnectionTracker::Selector& _Selector;
  DataPlaneCallbacks& _Callbacks;

  std::shared_ptr<Tun> _Tun;
  std::shared_ptr<UdpDynMux> _UdpDynMux;
  std::shared_ptr<VpnClientMultiChannel> _Client;

  std::set<VpnClientMultiChannel::Session*> _Endpoints;
  bool _Running = false;
};

} // namespace gh
