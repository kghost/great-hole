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

enum class TunnelState { Connecting, Connected, Disconnected, Failed };

class DataPlaneCallbacks {
public:
  virtual ~DataPlaneCallbacks() = default;
  virtual void UpdateState(TunnelState state, const std::string& message) = 0;
  virtual void OnTrafficStats(int64_t endpointHandle, uint64_t txBytes, uint64_t rxBytes) = 0;
};

class TunnelDataPlane {
public:
  TunnelDataPlane(boost::asio::any_io_executor executor, ConnectionTracker::SelectorType selector,
                  DataPlaneCallbacks& callbacks);
  ~TunnelDataPlane();

  TunnelDataPlane(const TunnelDataPlane&) = delete;
  TunnelDataPlane& operator=(const TunnelDataPlane&) = delete;
  TunnelDataPlane(TunnelDataPlane&&) = delete;
  TunnelDataPlane& operator=(TunnelDataPlane&&) = delete;

  Omni::Fiber::Coroutine<void> Start(int tunFd, int mtu, std::vector<char> encryptionKey);
  Omni::Fiber::Coroutine<void> Stop();
  Omni::Fiber::Coroutine<VpnClientMultiChannel::Session*> AddEndpoint(const UdpDynMux::PskType& psk,
                                                                      const std::string& host, int port);
  Omni::Fiber::Coroutine<void> RemoveEndpoint(VpnClientMultiChannel::Session* handle);

  std::optional<std::reference_wrapper<ConnectionMark>> FindSessionByHandle(VpnClientMultiChannel::Session* session);

  void ReportStats();

private:
  void StartStatsLoop();

  boost::asio::any_io_executor _Executor;
  ConnectionTracker::SelectorType& _Selector;
  DataPlaneCallbacks& _Callbacks;

  std::shared_ptr<Tun> _Tun;
  std::shared_ptr<UdpDynMux> _UdpDynMux;
  std::shared_ptr<VpnClientMultiChannel> _Client;
  std::shared_ptr<boost::asio::steady_timer> _StatsTimer;

  std::set<VpnClientMultiChannel::Session*> _Endpoints;
  bool _Running = false;
};

} // namespace gh
