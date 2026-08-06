#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>

#include "InterfaceCommonTypes.hpp"

#if defined(_WIN32)
#include "InterfaceWin32.hpp"
#endif

#if defined(_WIN32)
#if defined(GREAT_HOLE_WINDOWS_BUILD_DLL)
#define GREAT_HOLE_INTERFACE_API __declspec(dllexport)
#else
#define GREAT_HOLE_INTERFACE_API __declspec(dllimport)
#endif
#else
#define GREAT_HOLE_INTERFACE_API
#endif

namespace gh::Interface {

// Interface for the Tunnel Data Plane and Policy Engine
class PlatformInterface
#if defined(_WIN32)
    : public PlatformInterfaceWin32
#endif
{
public:
  explicit PlatformInterface() = default;
  virtual ~PlatformInterface() = default;

  PlatformInterface(const PlatformInterface&) = delete;
  auto operator=(const PlatformInterface&) -> PlatformInterface& = delete;
  PlatformInterface(PlatformInterface&&) = delete;
  auto operator=(PlatformInterface&&) -> PlatformInterface& = delete;

  virtual auto GetVersion() -> std::string;

  virtual auto StartEngine() -> std::error_code = 0;
  virtual auto StopEngine() -> std::error_code = 0;

  virtual auto StartVpn(std::span<IpAddress> addresses, int32_t mtu, std::span<uint8_t> encryption_key)
      -> std::error_code = 0;
  virtual auto StopVpn() -> std::error_code = 0;

  static constexpr size_t kPskSize = 16;
  using PskType = std::array<uint8_t, kPskSize>;
  virtual auto AddEndpoint(const PskType& psk, const std::string& address) -> VpnEndpoint = 0;
  virtual void RemoveEndpoint(VpnEndpoint endpoint) = 0;

  virtual void StartEndpoint(VpnEndpoint endpoint) = 0;
  virtual void StopEndpoint(VpnEndpoint endpoint) = 0;

  virtual auto GetTrafficStats(VpnEndpoint endpoint) -> std::optional<VpnTrafficStats> = 0;

  // Logging Interface
  virtual void SetLogLevel(LogLevel level) = 0;
};

GREAT_HOLE_INTERFACE_API auto CreatePlatform(DataPlaneCallbacks& callbacks) -> std::shared_ptr<PlatformInterface>;

} // namespace gh::Interface
