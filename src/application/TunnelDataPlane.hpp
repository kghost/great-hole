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
#include "EndpointUdpDynMux.hpp"
#include "Utils/Comparator.hpp"
#include "VpnClientMultiChannel.hpp"

#ifdef _WIN32
#include "EndpointWinDivert.hpp"
#endif

namespace gh {

enum class TunnelState : std::uint8_t { Starting = 0, Running = 1, Stopping = 2, Stopped = 3, Failed = 4 };

class DataPlaneCallbacks {
public:
  explicit DataPlaneCallbacks() = default;
  virtual ~DataPlaneCallbacks() = default;

  DataPlaneCallbacks(const DataPlaneCallbacks&) = delete;
  auto operator=(const DataPlaneCallbacks&) -> DataPlaneCallbacks& = delete;
  DataPlaneCallbacks(DataPlaneCallbacks&&) = delete;
  auto operator=(DataPlaneCallbacks&&) -> DataPlaneCallbacks& = delete;

  virtual void OnVpnStateChanged(TunnelState state, const std::string& message) = 0;
  virtual void OnTunnelStateChanged(const std::shared_ptr<VpnClientMultiChannel::Session>& session, TunnelState state,
                                    const std::string& error) = 0;
};

class TunnelDataPlane :
#ifdef _WIN32
    public WinDivertFastByPassCallback,
#endif
    public VpnClientMultiChannel::SessionStateListener {
public:
  TunnelDataPlane(boost::asio::any_io_executor executor, ConnectionTracker::Selector& selector,
                  DataPlaneCallbacks& callbacks);
  ~TunnelDataPlane();

  void OnSessionStarting(const std::shared_ptr<VpnClientMultiChannel::Session>& session) override;
  void OnSessionRunning(const std::shared_ptr<VpnClientMultiChannel::Session>& session) override;
  void OnSessionStopping(const std::shared_ptr<VpnClientMultiChannel::Session>& session) override;
  void OnSessionStopped(const std::shared_ptr<VpnClientMultiChannel::Session>& session) override;
  void OnSessionFailed(const std::shared_ptr<VpnClientMultiChannel::Session>& session,
                       const std::string& error) override;

  TunnelDataPlane(const TunnelDataPlane&) = delete;
  auto operator=(const TunnelDataPlane&) -> TunnelDataPlane& = delete;
  TunnelDataPlane(TunnelDataPlane&&) = delete;
  auto operator=(TunnelDataPlane&&) -> TunnelDataPlane& = delete;

#ifdef _WIN32
  auto Start(int mtu, std::vector<char> encryptionKey) -> Omni::Fiber::Coroutine<void>;
  auto ByPass(Packet& packet) -> bool override;
#else
  auto Start(int tunFd, int mtu, std::vector<char> encryptionKey) -> Omni::Fiber::Coroutine<void>;
  auto MigrateTun(int tunFd) -> Omni::Fiber::Coroutine<void>;
#endif
  auto Stop() -> Omni::Fiber::Coroutine<void>;
  auto AddEndpoint(const UdpDynMux::PskType& psk, const std::string& address)
      -> Omni::Fiber::Coroutine<std::shared_ptr<VpnClientMultiChannel::Session>>;
  auto RemoveEndpoint(std::shared_ptr<VpnClientMultiChannel::Session> session) -> Omni::Fiber::Coroutine<void>;

  auto FindSessionByHandle(VpnClientMultiChannel::Session* session) -> std::shared_ptr<VpnClientMultiChannel::Session>;

  static auto GetTrafficStats(const std::shared_ptr<VpnClientMultiChannel::Session>& session)
      -> std::optional<VpnTrafficStats>;

private:
  boost::asio::any_io_executor _Executor;
  ConnectionTracker::Selector& _Selector;
  DataPlaneCallbacks& _Callbacks;

  std::shared_ptr<VpnClientMultiChannel> _Client;

  std::set<std::shared_ptr<VpnClientMultiChannel::Session>, SharedPtrCompare> _Endpoints;
  bool _Running = false;
};

} // namespace gh
