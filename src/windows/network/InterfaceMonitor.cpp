#include "InterfaceMonitor.hpp"

#include <bit>
#include <expected>
#include <optional>
#include <utility>
#include <vector>

#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/log/trivial.hpp>

#include <windows.h>

#include <winsock2.h>

#include <iphlpapi.h>
#include <netioapi.h>
#include <ws2tcpip.h>

#include "Select.hpp"
#include "SelectPair.hpp"
#include "Strings.hpp"
#include "Utils/Overload.hpp"

namespace gh::windows::network {
namespace {
auto FormatAddress(const InterfaceMonitor::Address& address) -> std::string {
  return std::visit([](const auto& addr) -> std::string { return addr.to_string(); }, address);
}

auto GetInterfaceAlias(const NET_LUID& luid) -> std::optional<std::string> {
  std::array<WCHAR, IF_MAX_STRING_SIZE + 1> aliasBuffer{};
  if (ConvertInterfaceLuidToAlias(&luid, aliasBuffer.data(), std::size(aliasBuffer)) == NO_ERROR) {
    return ToString(std::wstring_view{aliasBuffer.data(), wcsnlen(aliasBuffer.data(), aliasBuffer.size())});
  }
  return std::nullopt;
}
} // namespace

InterfaceMonitor::InterfaceMonitor(boost::asio::any_io_executor executor) : _TaskQueue(std::move(executor)) {}

InterfaceMonitor::~InterfaceMonitor() {
  std::vector<InterfaceLuid> luidsToRestore;
  for (const auto& [luid, info] : _Interfaces) {
    bool hasOriginal = (info.V4Info.has_value() && info.V4Info->OriginalDnsServers.has_value()) ||
                       (info.V6Info.has_value() && info.V6Info->OriginalDnsServers.has_value());
    if (hasOriginal) {
      luidsToRestore.push_back(luid);
    }
  }
  for (auto luid : luidsToRestore) {
    auto result = RestoreDnsServers(luid);
    if (!result.has_value()) {
      BOOST_LOG_SEV(_Logger, boost::log::trivial::warning)
          << "Failed to restore DNS servers for interface LUID " << luid << " in destructor: " << result.error();
    }
  }
}

auto InterfaceMonitor::GetAddressInfo(const Address& address) const -> std::optional<IpAddressInfo> {
  auto iterator = _Addresses.find(address);
  if (iterator != _Addresses.end()) {
    return iterator->second;
  }
  return std::nullopt;
}

auto InterfaceMonitor::GetInterfaces(bool refresh) -> const std::map<InterfaceKey, InterfaceInfo>& {
  if (refresh) {
    Refresh();
  }
  return _Interfaces;
}

void InterfaceMonitor::Refresh() {
  auto newInterfaces = QueryIpInterfaces();

  for (auto& [key, newInfo] : newInterfaces) {
    auto oldIt = _Interfaces.find(key);
    if (oldIt != _Interfaces.end()) {
      if (newInfo.V4Info.has_value() && oldIt->second.V4Info.has_value()) {
        newInfo.V4Info->OriginalDnsServers = oldIt->second.V4Info->OriginalDnsServers;
      }
      if (newInfo.V6Info.has_value() && oldIt->second.V6Info.has_value()) {
        newInfo.V6Info->OriginalDnsServers = oldIt->second.V6Info->OriginalDnsServers;
      }
    }
  }

  _Interfaces = std::move(newInterfaces);
  _Addresses = QueryAddresses();
}

auto InterfaceMonitor::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> { co_return ErrorCode{}; }

auto InterfaceMonitor::DoWork() -> Omni::Fiber::Coroutine<void> {
  HANDLE addressNotifyHandle{nullptr};
  HANDLE interfaceNotifyHandle{nullptr};

  NotifyUnicastIpAddressChange(AF_UNSPEC, &OnUnicastIpAddressChangedCallback, this, FALSE, &addressNotifyHandle);
  NotifyIpInterfaceChange(AF_UNSPEC, &OnIpInterfaceChangedCallback, this, FALSE, &interfaceNotifyHandle);
  Refresh();

  while (true) {
    auto [cancel, hasTask] = co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(), [] -> void {}),
        Omni::Fiber::SelectPair(_TaskQueue, [] -> void {}));

    if (hasTask) {
      while (!_TaskQueue.IsEmpty()) {
        auto task = _TaskQueue.PopFront();
        std::visit(Overload{
                       [this](AddressChangeEvent& event) -> void { HandleAddressChangeEvent(event); },
                       [this](InterfaceChangeEvent& event) -> void { HandleInterfaceChangeEvent(event); },
                   },
                   task.Event);
        task.CompletionPromise.set_value();
      }
    }

    if (cancel) {
      break;
    }
  }

  if (interfaceNotifyHandle != nullptr) {
    CancelMibChangeNotify2(interfaceNotifyHandle);
    interfaceNotifyHandle = nullptr;
  }
  if (addressNotifyHandle != nullptr) {
    CancelMibChangeNotify2(addressNotifyHandle);
    addressNotifyHandle = nullptr;
  }
  while (!_TaskQueue.IsEmpty()) {
    auto task = _TaskQueue.PopFront();
    task.CompletionPromise.set_value();
  }
}

auto InterfaceMonitor::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> { co_return ErrorCode{}; }

auto InterfaceMonitor::ConvertSockaddrInet(const SOCKADDR_INET& addr) -> std::optional<Address> {
  if (addr.si_family == AF_INET) {
    auto address =
        boost::asio::ip::address_v4(std::bit_cast<boost::asio::ip::address_v4::bytes_type>(addr.Ipv4.sin_addr));
    return address;
  } else if (addr.si_family == AF_INET6) {
    auto address =
        boost::asio::ip::address_v6(std::bit_cast<boost::asio::ip::address_v6::bytes_type>(addr.Ipv6.sin6_addr));
    return address;
  } else {
    return std::nullopt;
  }
}

namespace {
void RemoveAddressFromInterface(std::map<InterfaceMonitor::InterfaceKey, InterfaceMonitor::InterfaceInfo>& interfaces,
                                const InterfaceMonitor::Address& address, InterfaceMonitor::InterfaceKey luidValue) {
  auto iterator = interfaces.find(luidValue);
  if (iterator == interfaces.end()) {
    return;
  }
  if (std::holds_alternative<boost::asio::ip::address_v4>(address)) {
    const auto& address4 = std::get<boost::asio::ip::address_v4>(address);
    Interface::Ip4Address target4{.Bytes = address4.to_bytes()};
    if (iterator->second.V4Info.has_value()) {
      std::erase_if(iterator->second.V4Info->Addresses,
                    [&](const auto& item) -> bool { return item.Address == target4; });
    }
  } else if (std::holds_alternative<boost::asio::ip::address_v6>(address)) {
    const auto& address6 = std::get<boost::asio::ip::address_v6>(address);
    Interface::Ip6Address target6{.Bytes = address6.to_bytes()};
    if (iterator->second.V6Info.has_value()) {
      std::erase_if(iterator->second.V6Info->Addresses,
                    [&](const auto& item) -> bool { return item.Address == target6; });
    }
  }
}

void AddAddressToInterface(std::map<InterfaceMonitor::InterfaceKey, InterfaceMonitor::InterfaceInfo>& interfaces,
                           const InterfaceMonitor::Address& address, uint8_t prefixLength,
                           InterfaceMonitor::InterfaceKey luidValue, NET_IFINDEX ifIndex) {
  auto iterator = interfaces.find(luidValue);
  if (iterator == interfaces.end()) {
    NET_LUID luid{};
    luid.Value = luidValue;
    iterator = interfaces
                   .try_emplace(luidValue, InterfaceMonitor::InterfaceInfo{.InterfaceIndex = ifIndex,
                                                                           .InterfaceLuid = luidValue,
                                                                           .InterfaceName = GetInterfaceAlias(luid)})
                   .first;
  }
  if (std::holds_alternative<boost::asio::ip::address_v4>(address)) {
    const auto& address4 = std::get<boost::asio::ip::address_v4>(address);
    Interface::Ip4Address target4{.Bytes = address4.to_bytes()};
    if (!iterator->second.V4Info.has_value()) {
      iterator->second.V4Info.emplace();
    }
    iterator->second.V4Info->Addresses.insert(
        InterfaceMonitor::InterfaceAddress<Interface::Ip4Address>{.Address = target4, .PrefixLength = prefixLength});
  } else if (std::holds_alternative<boost::asio::ip::address_v6>(address)) {
    const auto& address6 = std::get<boost::asio::ip::address_v6>(address);
    Interface::Ip6Address target6{.Bytes = address6.to_bytes()};
    if (!iterator->second.V6Info.has_value()) {
      iterator->second.V6Info.emplace();
    }
    iterator->second.V6Info->Addresses.insert(
        InterfaceMonitor::InterfaceAddress<Interface::Ip6Address>{.Address = target6, .PrefixLength = prefixLength});
  }
}

void AddDnsServerToInterface(std::map<InterfaceMonitor::InterfaceKey, InterfaceMonitor::InterfaceInfo>& interfaces,
                             const InterfaceMonitor::Address& address, InterfaceMonitor::InterfaceKey luidValue,
                             NET_IFINDEX ifIndex) {
  auto iterator = interfaces.find(luidValue);
  if (iterator == interfaces.end()) {
    NET_LUID luid{};
    luid.Value = luidValue;
    iterator = interfaces
                   .try_emplace(luidValue, InterfaceMonitor::InterfaceInfo{.InterfaceIndex = ifIndex,
                                                                           .InterfaceLuid = luidValue,
                                                                           .InterfaceName = GetInterfaceAlias(luid)})
                   .first;
  }
  if (std::holds_alternative<boost::asio::ip::address_v4>(address)) {
    const auto& address4 = std::get<boost::asio::ip::address_v4>(address);
    Interface::Ip4Address target4{.Bytes = address4.to_bytes()};
    if (!iterator->second.V4Info.has_value()) {
      iterator->second.V4Info.emplace();
    }
    if (std::ranges::find(iterator->second.V4Info->DnsServers.Servers, target4) ==
        iterator->second.V4Info->DnsServers.Servers.end()) {
      iterator->second.V4Info->DnsServers.Servers.push_back(target4);
    }
  } else if (std::holds_alternative<boost::asio::ip::address_v6>(address)) {
    const auto& address6 = std::get<boost::asio::ip::address_v6>(address);
    Interface::Ip6Address target6{.Bytes = address6.to_bytes()};
    if (!iterator->second.V6Info.has_value()) {
      iterator->second.V6Info.emplace();
    }
    if (std::ranges::find(iterator->second.V6Info->DnsServers.Servers, target6) ==
        iterator->second.V6Info->DnsServers.Servers.end()) {
      iterator->second.V6Info->DnsServers.Servers.push_back(target6);
    }
  }
}
} // namespace

auto InterfaceMonitor::HandleAddressChangeEvent(AddressChangeEvent& event) -> bool {
  auto ipOpt = ConvertSockaddrInet(event.Row.Address);
  if (!ipOpt.has_value()) {
    return false;
  }
  const auto& address = ipOpt.value();

  switch (event.NotificationType) {
  case MibInitialNotification: {
    return false;
  }
  case MibAddInstance: {
    auto err = GetUnicastIpAddressEntry(&event.Row);
    if (err != NO_ERROR) {
      BOOST_LOG_SEV(_Logger, boost::log::trivial::warning)
          << "Failed to get unicast IP address entry for address change event: " << err;
      return false;
    }

    NET_LUID newLuid = event.Row.InterfaceLuid;
    if (newLuid.Value == 0) {
      ConvertInterfaceIndexToLuid(event.Row.InterfaceIndex, &newLuid);
    }

    auto iterator = _Addresses.find(address);
    if (iterator != _Addresses.end()) {
      auto oldIfIndex = iterator->second.InterfaceIndex;
      NET_LUID oldLuid{};
      ConvertInterfaceIndexToLuid(oldIfIndex, &oldLuid);
      RemoveAddressFromInterface(_Interfaces, address, oldLuid.Value);
      if (oldLuid.Value != newLuid.Value) {
        RemoveAddressFromInterface(_Interfaces, address, newLuid.Value);
      }
      iterator->second.PrefixLength = event.Row.OnLinkPrefixLength;
      iterator->second.InterfaceIndex = event.Row.InterfaceIndex;
      AddAddressToInterface(_Interfaces, address, event.Row.OnLinkPrefixLength, newLuid.Value,
                            event.Row.InterfaceIndex);
      BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
          << "Address " << FormatAddress(address) << "/" << static_cast<int>(iterator->second.PrefixLength)
          << " updated on interface, LUID " << newLuid.Value << " (index " << iterator->second.InterfaceIndex << ")";
      return true;
    } else {
      _Addresses.try_emplace(address, IpAddressInfo{.PrefixLength = event.Row.OnLinkPrefixLength,
                                                    .InterfaceIndex = event.Row.InterfaceIndex});
      AddAddressToInterface(_Interfaces, address, event.Row.OnLinkPrefixLength, newLuid.Value,
                            event.Row.InterfaceIndex);
      BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
          << "Address " << FormatAddress(address) << "/" << static_cast<int>(event.Row.OnLinkPrefixLength)
          << " added on interface, LUID " << newLuid.Value << " (index " << event.Row.InterfaceIndex << ")";
      return true;
    }
  }
  case MibDeleteInstance: {
    auto iterator = _Addresses.find(address);
    if (iterator != _Addresses.end()) {
      auto oldIfIndex = iterator->second.InterfaceIndex;
      NET_LUID oldLuid{};
      ConvertInterfaceIndexToLuid(oldIfIndex, &oldLuid);
      RemoveAddressFromInterface(_Interfaces, address, oldLuid.Value);
      NET_LUID eventLuid = event.Row.InterfaceLuid;
      if (eventLuid.Value != 0 && eventLuid.Value != oldLuid.Value) {
        RemoveAddressFromInterface(_Interfaces, address, eventLuid.Value);
      }
      _Addresses.erase(iterator);
      BOOST_LOG_SEV(_Logger, boost::log::trivial::info) << "Address " << FormatAddress(address) << " removed";
      return true;
    } else {
      NET_LUID eventLuid = event.Row.InterfaceLuid;
      if (eventLuid.Value == 0) {
        ConvertInterfaceIndexToLuid(event.Row.InterfaceIndex, &eventLuid);
      }
      RemoveAddressFromInterface(_Interfaces, address, eventLuid.Value);
      return false;
    }
  }
  case MibParameterNotification: {
    auto err = GetUnicastIpAddressEntry(&event.Row);
    if (err != NO_ERROR) {
      BOOST_LOG_SEV(_Logger, boost::log::trivial::warning)
          << "Failed to get unicast IP address entry for address change event: " << err;
      return false;
    }

    auto iterator = _Addresses.find(address);
    if (iterator == _Addresses.end()) {
      return false;
    }
    NET_LUID newLuid = event.Row.InterfaceLuid;
    if (newLuid.Value == 0) {
      ConvertInterfaceIndexToLuid(event.Row.InterfaceIndex, &newLuid);
    }
    if (iterator->second.PrefixLength != event.Row.OnLinkPrefixLength ||
        iterator->second.InterfaceIndex != event.Row.InterfaceIndex) {
      NET_LUID oldLuid{};
      ConvertInterfaceIndexToLuid(iterator->second.InterfaceIndex, &oldLuid);
      RemoveAddressFromInterface(_Interfaces, address, oldLuid.Value);
      RemoveAddressFromInterface(_Interfaces, address, newLuid.Value);
      iterator->second.PrefixLength = event.Row.OnLinkPrefixLength;
      iterator->second.InterfaceIndex = event.Row.InterfaceIndex;
      AddAddressToInterface(_Interfaces, address, event.Row.OnLinkPrefixLength, newLuid.Value,
                            event.Row.InterfaceIndex);
      BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
          << "Address " << FormatAddress(address) << "/" << static_cast<int>(iterator->second.PrefixLength)
          << " updated on interface, LUID " << newLuid.Value << " (index " << iterator->second.InterfaceIndex << ")";
      return true;
    }
    AddAddressToInterface(_Interfaces, address, event.Row.OnLinkPrefixLength, newLuid.Value, event.Row.InterfaceIndex);
    return false;
  }
  default:
    return false;
  }
}

void InterfaceMonitor::HandleInterfaceChangeEvent(InterfaceChangeEvent& event) {
  switch (event.NotificationType) {
  case MibInitialNotification: {
    return;
  }
  case MibAddInstance:
  case MibParameterNotification: {
    auto err = GetIpInterfaceEntry(&event.Row);
    if (err != NO_ERROR) {
      BOOST_LOG_SEV(_Logger, boost::log::trivial::warning)
          << "Failed to get IP interface entry for interface change event: " << err;
      return;
    }

    auto [iterator, inserted] = _Interfaces.try_emplace(
        event.Row.InterfaceLuid.Value, InterfaceInfo{.InterfaceIndex = event.Row.InterfaceIndex,
                                                     .InterfaceLuid = event.Row.InterfaceLuid.Value,
                                                     .InterfaceName = GetInterfaceAlias(event.Row.InterfaceLuid)});

    if (!inserted) {
      iterator->second.InterfaceLuid = event.Row.InterfaceLuid.Value;
      if (iterator->second.InterfaceIndex == 0) {
        iterator->second.InterfaceIndex = event.Row.InterfaceIndex;
      }
      if (!iterator->second.InterfaceName.has_value()) {
        iterator->second.InterfaceName = GetInterfaceAlias(event.Row.InterfaceLuid);
      }
    }

    if (event.Row.Family == AF_INET) {
      IpInterfaceCommon newIpInfo{
          .NlMtu = event.Row.NlMtu,
          .Metric = event.Row.Metric,
          .Connected = event.Row.Connected != FALSE,
          .ForwardingEnabled = event.Row.ForwardingEnabled != FALSE,
      };
      auto& targetInfo = iterator->second.V4Info;
      bool changed = inserted || !targetInfo.has_value() || targetInfo.value().Common.NlMtu != newIpInfo.NlMtu ||
                     targetInfo.value().Common.Metric != newIpInfo.Metric ||
                     targetInfo.value().Common.Connected != newIpInfo.Connected ||
                     targetInfo.value().Common.ForwardingEnabled != newIpInfo.ForwardingEnabled;
      if (!targetInfo.has_value()) {
        targetInfo = IpInterfaceInfo<Interface::Ip4Address>{.Common = newIpInfo};
      } else {
        targetInfo.value().Common.NlMtu = newIpInfo.NlMtu;
        targetInfo.value().Common.Metric = newIpInfo.Metric;
        targetInfo.value().Common.Connected = newIpInfo.Connected;
        targetInfo.value().Common.ForwardingEnabled = newIpInfo.ForwardingEnabled;
      }
      if (changed) {
        BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
            << "Interface " << event.Row.InterfaceIndex << " (" << iterator->second.InterfaceName.value_or("Unknown")
            << ", IPv4) updated: MTU=" << newIpInfo.NlMtu << ", Metric=" << newIpInfo.Metric
            << ", Connected=" << (newIpInfo.Connected ? "Yes" : "No")
            << ", Forwarding=" << (newIpInfo.ForwardingEnabled ? "Yes" : "No");
      }
    } else if (event.Row.Family == AF_INET6) {
      IpInterfaceCommon newIpInfo{
          .NlMtu = event.Row.NlMtu,
          .Metric = event.Row.Metric,
          .Connected = event.Row.Connected != FALSE,
          .ForwardingEnabled = event.Row.ForwardingEnabled != FALSE,
      };
      auto& targetInfo = iterator->second.V6Info;
      bool changed = inserted || !targetInfo.has_value() || targetInfo.value().Common.NlMtu != newIpInfo.NlMtu ||
                     targetInfo.value().Common.Metric != newIpInfo.Metric ||
                     targetInfo.value().Common.Connected != newIpInfo.Connected ||
                     targetInfo.value().Common.ForwardingEnabled != newIpInfo.ForwardingEnabled;
      if (!targetInfo.has_value()) {
        targetInfo = IpInterfaceInfo<Interface::Ip6Address>{.Common = newIpInfo};
      } else {
        targetInfo.value().Common.NlMtu = newIpInfo.NlMtu;
        targetInfo.value().Common.Metric = newIpInfo.Metric;
        targetInfo.value().Common.Connected = newIpInfo.Connected;
        targetInfo.value().Common.ForwardingEnabled = newIpInfo.ForwardingEnabled;
      }
      if (changed) {
        BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
            << "Interface " << event.Row.InterfaceIndex << " (" << iterator->second.InterfaceName.value_or("Unknown")
            << ", IPv6) updated: MTU=" << newIpInfo.NlMtu << ", Metric=" << newIpInfo.Metric
            << ", Connected=" << (newIpInfo.Connected ? "Yes" : "No")
            << ", Forwarding=" << (newIpInfo.ForwardingEnabled ? "Yes" : "No");
      }
    }
    return;
  }
  case MibDeleteInstance: {
    NET_LUID luid = event.Row.InterfaceLuid;
    if (luid.Value == 0) {
      ConvertInterfaceIndexToLuid(event.Row.InterfaceIndex, &luid);
    }
    auto iterator = _Interfaces.find(luid.Value);
    if (iterator == _Interfaces.end()) {
      return;
    }

    if (event.Row.Family == AF_INET) {
      iterator->second.V4Info.reset();
    } else if (event.Row.Family == AF_INET6) {
      iterator->second.V6Info.reset();
    }

    if (!iterator->second.V4Info.has_value() && !iterator->second.V6Info.has_value()) {
      _Interfaces.erase(iterator);
      BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
          << "Interface LUID " << luid.Value << " (index " << event.Row.InterfaceIndex << ", "
          << (event.Row.Family == AF_INET ? "IPv4" : "IPv6") << ") fully removed";
    } else {
      BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
          << "Interface LUID " << luid.Value << " (index " << event.Row.InterfaceIndex << ", "
          << (event.Row.Family == AF_INET ? "IPv4" : "IPv6") << ") address family removed";
    }
    return;
  }
  default:
    return;
  }
}

auto InterfaceMonitor::QueryAddresses() -> std::map<Address, IpAddressInfo> {
  std::map<Address, IpAddressInfo> result;

  for (auto& [key, info] : _Interfaces) {
    if (info.V4Info.has_value()) {
      info.V4Info->Addresses.clear();
      info.V4Info->DnsServers.Servers.clear();
    }
    if (info.V6Info.has_value()) {
      info.V6Info->Addresses.clear();
      info.V6Info->DnsServers.Servers.clear();
    }
  }

  ULONG flags =
      GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_FRIENDLY_NAME | GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_ALL_INTERFACES;
  ULONG family = AF_UNSPEC;
  ULONG outBufLen = 15360; // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
  std::vector<BYTE> buffer(outBufLen);

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto* pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
  ULONG dwRetVal = GetAdaptersAddresses(family, flags, nullptr, pAddresses, &outBufLen);
  if (dwRetVal == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(outBufLen);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    dwRetVal = GetAdaptersAddresses(family, flags, nullptr, pAddresses, &outBufLen);
  }

  if (dwRetVal != NO_ERROR) {
    return result;
  }

  for (const auto* pCurr = pAddresses; pCurr != nullptr; pCurr = pCurr->Next) {
    NET_LUID luid = pCurr->Luid;
    if (luid.Value == 0) {
      ConvertInterfaceIndexToLuid(pCurr->IfIndex, &luid);
    }

    auto it = _Interfaces.find(luid.Value);
    if (it != _Interfaces.end()) {
      if (it->second.V4Info.has_value()) {
        it->second.V4Info->DnsServers.IsDhcp = (pCurr->Dhcpv4Enabled != 0);
      }
      if (it->second.V6Info.has_value()) {
        it->second.V6Info->DnsServers.IsDhcp =
            (pCurr->Ipv6OtherStatefulConfig != 0 || pCurr->Ipv6ManagedAddressConfigurationSupported != 0 ||
             pCurr->Dhcpv6Server.lpSockaddr != nullptr);
      }
    }

    // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)
    NET_IFINDEX v4IfIndex = pCurr->IfIndex;
    NET_IFINDEX v6IfIndex = (pCurr->Ipv6IfIndex != 0) ? pCurr->Ipv6IfIndex : pCurr->IfIndex;
    // NOLINTEND(cppcoreguidelines-pro-type-union-access)

    for (const auto* pUnicast = pCurr->FirstUnicastAddress; pUnicast != nullptr; pUnicast = pUnicast->Next) {
      if (pUnicast->Address.lpSockaddr == nullptr) {
        continue;
      }
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      auto addressOpt = ConvertSockaddrInet(*reinterpret_cast<const SOCKADDR_INET*>(pUnicast->Address.lpSockaddr));
      if (!addressOpt.has_value()) {
        continue;
      }
      const auto& address = addressOpt.value();
      NET_IFINDEX ifIndex = std::holds_alternative<boost::asio::ip::address_v6>(address) ? v6IfIndex : v4IfIndex;

      auto [iterator, inserted] = result.try_emplace(address, IpAddressInfo{
                                                                  .PrefixLength = pUnicast->OnLinkPrefixLength,
                                                                  .InterfaceIndex = ifIndex,
                                                              });
      AddAddressToInterface(_Interfaces, address, pUnicast->OnLinkPrefixLength, luid.Value, ifIndex);

      if (inserted) {
        BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
            << "Address " << FormatAddress(address) << "/" << static_cast<int>(iterator->second.PrefixLength)
            << " discovered on interface, " << iterator->second.InterfaceIndex;
        continue;
      } else {
        BOOST_LOG_SEV(_Logger, boost::log::trivial::warning)
            << "Address " << FormatAddress(address) << " already exists on different interface, " << ifIndex << " vs "
            << iterator->second.InterfaceIndex;
      }
    }

    for (const auto* pDns = pCurr->FirstDnsServerAddress; pDns != nullptr; pDns = pDns->Next) {
      if (pDns->Address.lpSockaddr == nullptr) {
        continue;
      }
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      auto addressOpt = ConvertSockaddrInet(*reinterpret_cast<const SOCKADDR_INET*>(pDns->Address.lpSockaddr));
      if (!addressOpt.has_value()) {
        continue;
      }
      const auto& address = addressOpt.value();
      NET_IFINDEX ifIndex = std::holds_alternative<boost::asio::ip::address_v6>(address) ? v6IfIndex : v4IfIndex;

      AddDnsServerToInterface(_Interfaces, address, luid.Value, ifIndex);
      BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
          << "DNS server " << FormatAddress(address) << " discovered on interface, " << ifIndex;
    }
  }

  return result;
}

auto InterfaceMonitor::QueryIpInterfaces() -> std::map<InterfaceKey, InterfaceInfo> {
  std::map<InterfaceKey, InterfaceInfo> result;
  PMIB_IPINTERFACE_TABLE pTable = nullptr;
  ULONG dwRetVal = GetIpInterfaceTable(AF_UNSPEC, &pTable);
  if (dwRetVal != NO_ERROR || pTable == nullptr) {
    BOOST_LOG_SEV(_Logger, boost::log::trivial::warning) << "Failed to query IP interfaces: " << dwRetVal;
    return result;
  }

  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
  for (const auto& row : std::span(pTable->Table, pTable->NumEntries)) {
    InterfaceKey key = row.InterfaceLuid.Value;
    auto [iterator, inserted] =
        result.try_emplace(key, InterfaceInfo{.InterfaceIndex = row.InterfaceIndex,
                                              .InterfaceLuid = row.InterfaceLuid.Value,
                                              .InterfaceName = GetInterfaceAlias(row.InterfaceLuid)});
    if (!inserted) {
      if (iterator->second.InterfaceIndex == 0) {
        iterator->second.InterfaceIndex = row.InterfaceIndex;
      }
      if (!iterator->second.InterfaceName.has_value()) {
        iterator->second.InterfaceName = GetInterfaceAlias(row.InterfaceLuid);
      }
    }
    IpInterfaceCommon ipInfo{
        .NlMtu = row.NlMtu,
        .Metric = row.Metric,
        .Connected = row.Connected != FALSE,
        .ForwardingEnabled = row.ForwardingEnabled != FALSE,
    };

    if (row.Family == AF_INET) {
      if (!iterator->second.V4Info.has_value()) {
        iterator->second.V4Info = IpInterfaceInfo<Interface::Ip4Address>{.Common = ipInfo};
      } else {
        BOOST_LOG_SEV(_Logger, boost::log::trivial::warning)
            << "Duplicate IPv4 address information for interface " << row.InterfaceIndex << " ("
            << iterator->second.InterfaceName.value_or("Unknown") << ", " << (row.Family == AF_INET ? "IPv4" : "IPv6")
            << ")";
      }
    } else if (row.Family == AF_INET6) {
      if (!iterator->second.V6Info.has_value()) {
        iterator->second.V6Info = IpInterfaceInfo<Interface::Ip6Address>{.Common = ipInfo};
      } else {
        BOOST_LOG_SEV(_Logger, boost::log::trivial::warning)
            << "Duplicate IPv6 address information for interface " << row.InterfaceIndex << " ("
            << iterator->second.InterfaceName.value_or("Unknown") << ", " << (row.Family == AF_INET ? "IPv4" : "IPv6")
            << ")";
      }
    }

    BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
        << "Discovered interface " << row.InterfaceIndex << " (" << iterator->second.InterfaceName.value_or("Unknown")
        << ", " << (row.Family == AF_INET ? "IPv4" : "IPv6") << ", MTU: " << row.NlMtu << ", Metric: " << row.Metric
        << ", Connected: " << (row.Connected != FALSE ? "Yes" : "No") << ")";
  }

  FreeMibTable(pTable);
  return result;
}

void WINAPI InterfaceMonitor::OnUnicastIpAddressChangedCallback(PVOID context, PMIB_UNICASTIPADDRESS_ROW row,
                                                                MIB_NOTIFICATION_TYPE notificationType) {
  auto* self = static_cast<InterfaceMonitor*>(context);
  if (self == nullptr || row == nullptr) {
    return;
  }
  std::promise<void> promise;
  auto future = promise.get_future();
  self->_TaskQueue.Push(Task{
      .Event = AddressChangeEvent{.NotificationType = notificationType, .Row = *row},
      .CompletionPromise = std::move(promise),
  });
  future.wait();
}

void WINAPI InterfaceMonitor::OnIpInterfaceChangedCallback(PVOID context, PMIB_IPINTERFACE_ROW row,
                                                           MIB_NOTIFICATION_TYPE notificationType) {
  auto* self = static_cast<InterfaceMonitor*>(context);
  if (self == nullptr || row == nullptr) {
    return;
  }
  std::promise<void> promise;
  auto future = promise.get_future();
  self->_TaskQueue.Push(Task{
      .Event = InterfaceChangeEvent{.NotificationType = notificationType, .Row = *row},
      .CompletionPromise = std::move(promise),
  });
  future.wait();
}

auto InterfaceMonitor::OverrideDnsServers(InterfaceLuid interfaceLuid,
                                          const std::vector<Interface::IpAddress>& dnsServers)
    -> std::expected<void, std::string> {
  if (dnsServers.empty()) {
    return std::unexpected("No DNS servers provided to override for interface LUID " + std::to_string(interfaceLuid));
  }

  auto iterator = _Interfaces.find(interfaceLuid);
  if (iterator == _Interfaces.end()) {
    NET_LUID luid{};
    luid.Value = interfaceLuid;
    NET_IFINDEX ifIndex = 0;
    ConvertInterfaceLuidToIndex(&luid, &ifIndex);
    iterator = _Interfaces
                   .try_emplace(interfaceLuid, InterfaceInfo{.InterfaceIndex = ifIndex,
                                                             .InterfaceLuid = interfaceLuid,
                                                             .InterfaceName = GetInterfaceAlias(luid)})
                   .first;
  }

  NET_LUID luid{};
  luid.Value = interfaceLuid;
  GUID guid{};
  if (ConvertInterfaceLuidToGuid(&luid, &guid) != NO_ERROR) {
    BOOST_LOG_SEV(_Logger, boost::log::trivial::warning)
        << "Failed to convert LUID to GUID for interface LUID " << interfaceLuid;
    return std::unexpected("Failed to convert LUID to GUID for interface LUID " + std::to_string(interfaceLuid));
  }

  std::vector<Interface::Ip4Address> v4Servers;
  std::vector<Interface::Ip6Address> v6Servers;
  for (const auto& addr : dnsServers) {
    if (std::holds_alternative<Interface::Ip4Address>(addr)) {
      v4Servers.push_back(std::get<Interface::Ip4Address>(addr));
    } else if (std::holds_alternative<Interface::Ip6Address>(addr)) {
      v6Servers.push_back(std::get<Interface::Ip6Address>(addr));
    }
  }

  std::string errorMsg;

  if (!v4Servers.empty()) {
    auto& targetInfo = iterator->second.V4Info;
    if (!targetInfo.has_value()) {
      targetInfo.emplace();
    }
    if (!targetInfo->OriginalDnsServers.has_value()) {
      targetInfo->OriginalDnsServers = targetInfo->DnsServers;
    }

    std::wstring dnsString;
    for (size_t i = 0; i < v4Servers.size(); ++i) {
      if (i > 0) {
        dnsString += L",";
      }
      auto ipStr = boost::asio::ip::address_v4(v4Servers[i].Bytes).to_string();
      dnsString += std::wstring(ipStr.begin(), ipStr.end());
    }

    DNS_INTERFACE_SETTINGS settings{};
    settings.Version = DNS_INTERFACE_SETTINGS_VERSION1;
    settings.Flags = DNS_SETTING_NAMESERVER;
    settings.NameServer = dnsString.data();

    DWORD status = SetInterfaceDnsSettings(guid, &settings);
    if (status != NO_ERROR) {
      BOOST_LOG_SEV(_Logger, boost::log::trivial::warning)
          << "SetInterfaceDnsSettings failed for interface LUID " << interfaceLuid << " (IPv4) with error: " << status;
      errorMsg += "SetInterfaceDnsSettings IPv4 failed for interface LUID " + std::to_string(interfaceLuid) +
                  " with error: " + std::to_string(status) + "; ";
    } else {
      targetInfo->DnsServers.Servers = v4Servers;
      targetInfo->DnsServers.IsDhcp = false;
    }
  }

  if (!v6Servers.empty()) {
    auto& targetInfo = iterator->second.V6Info;
    if (!targetInfo.has_value()) {
      targetInfo.emplace();
    }
    if (!targetInfo->OriginalDnsServers.has_value()) {
      targetInfo->OriginalDnsServers = targetInfo->DnsServers;
    }

    std::wstring dnsString;
    for (size_t i = 0; i < v6Servers.size(); ++i) {
      if (i > 0) {
        dnsString += L",";
      }
      auto ipStr = boost::asio::ip::address_v6(v6Servers[i].Bytes).to_string();
      dnsString += std::wstring(ipStr.begin(), ipStr.end());
    }

    DNS_INTERFACE_SETTINGS settings{};
    settings.Version = DNS_INTERFACE_SETTINGS_VERSION1;
    settings.Flags = DNS_SETTING_NAMESERVER | DNS_SETTING_IPV6;
    settings.NameServer = dnsString.data();

    DWORD status = SetInterfaceDnsSettings(guid, &settings);
    if (status != NO_ERROR) {
      BOOST_LOG_SEV(_Logger, boost::log::trivial::warning)
          << "SetInterfaceDnsSettings failed for interface LUID " << interfaceLuid << " (IPv6) with error: " << status;
      errorMsg += "SetInterfaceDnsSettings IPv6 failed for interface LUID " + std::to_string(interfaceLuid) +
                  " with error: " + std::to_string(status) + "; ";
    } else {
      targetInfo->DnsServers.Servers = v6Servers;
      targetInfo->DnsServers.IsDhcp = false;
    }
  }

  if (!errorMsg.empty()) {
    return std::unexpected(errorMsg);
  }
  return {};
}

auto InterfaceMonitor::RestoreDnsServers(InterfaceLuid interfaceLuid) -> std::expected<void, std::string> {
  auto iterator = _Interfaces.find(interfaceLuid);
  if (iterator == _Interfaces.end()) {
    return std::unexpected("Interface LUID " + std::to_string(interfaceLuid) + " not found");
  }

  bool hasOriginal = (iterator->second.V4Info.has_value() && iterator->second.V4Info->OriginalDnsServers.has_value()) ||
                     (iterator->second.V6Info.has_value() && iterator->second.V6Info->OriginalDnsServers.has_value());
  if (!hasOriginal) {
    return std::unexpected("No original DNS servers saved for interface LUID " + std::to_string(interfaceLuid));
  }

  std::string errorMsg;
  NET_LUID luid{};
  luid.Value = interfaceLuid;
  GUID guid{};
  if (ConvertInterfaceLuidToGuid(&luid, &guid) != NO_ERROR) {
    return std::unexpected("ConvertInterfaceLuidToGuid failed for interface LUID " + std::to_string(interfaceLuid));
  }

  if (iterator->second.V4Info.has_value() && iterator->second.V4Info->OriginalDnsServers.has_value()) {
    auto originalDnsInfo = iterator->second.V4Info->OriginalDnsServers.value();

    std::wstring dnsString;
    if (!originalDnsInfo.IsDhcp) {
      for (size_t i = 0; i < originalDnsInfo.Servers.size(); ++i) {
        if (i > 0) {
          dnsString += L",";
        }
        auto ipStr = boost::asio::ip::address_v4(originalDnsInfo.Servers[i].Bytes).to_string();
        dnsString += std::wstring(ipStr.begin(), ipStr.end());
      }
    }

    DNS_INTERFACE_SETTINGS settings{};
    settings.Version = DNS_INTERFACE_SETTINGS_VERSION1;
    settings.Flags = DNS_SETTING_NAMESERVER;
    settings.NameServer = dnsString.data();

    DWORD status = SetInterfaceDnsSettings(guid, &settings);
    if (status != NO_ERROR) {
      BOOST_LOG_SEV(_Logger, boost::log::trivial::warning)
          << "SetInterfaceDnsSettings failed for interface LUID " << interfaceLuid << " (IPv4) with error: " << status;
      errorMsg += "SetInterfaceDnsSettings IPv4 failed with error " + std::to_string(status) + "; ";
    }

    iterator->second.V4Info->OriginalDnsServers.reset();
  }

  if (iterator->second.V6Info.has_value() && iterator->second.V6Info->OriginalDnsServers.has_value()) {
    auto originalDnsInfo = iterator->second.V6Info->OriginalDnsServers.value();

    std::wstring dnsString;
    if (!originalDnsInfo.IsDhcp) {
      for (size_t i = 0; i < originalDnsInfo.Servers.size(); ++i) {
        if (i > 0) {
          dnsString += L",";
        }
        auto ipStr = boost::asio::ip::address_v6(originalDnsInfo.Servers[i].Bytes).to_string();
        dnsString += std::wstring(ipStr.begin(), ipStr.end());
      }
    }

    DNS_INTERFACE_SETTINGS settings{};
    settings.Version = DNS_INTERFACE_SETTINGS_VERSION1;
    settings.Flags = DNS_SETTING_NAMESERVER | DNS_SETTING_IPV6;
    settings.NameServer = dnsString.data();

    DWORD status = SetInterfaceDnsSettings(guid, &settings);
    if (status != NO_ERROR) {
      BOOST_LOG_SEV(_Logger, boost::log::trivial::warning)
          << "SetInterfaceDnsSettings failed for interface LUID " << interfaceLuid << " (IPv6) with error: " << status;
      errorMsg += "SetInterfaceDnsSettings IPv6 failed with error " + std::to_string(status) + "; ";
    }

    iterator->second.V6Info->OriginalDnsServers.reset();
  }

  if (!errorMsg.empty()) {
    return std::unexpected(errorMsg);
  }
  return {};
}

} // namespace gh::windows::network
