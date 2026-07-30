#include "InterfaceMonitor.hpp"

#include <bit>
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

namespace gh::windows::network {
namespace {
auto FormatAddress(const InterfaceMonitor::Address& address) -> std::string {
  return std::visit([](const auto& addr) -> std::string { return addr.to_string(); }, address);
}
} // namespace

InterfaceMonitor::InterfaceMonitor(boost::asio::any_io_executor executor) : _TaskQueue(std::move(executor)) {}

InterfaceMonitor::~InterfaceMonitor() {}

auto InterfaceMonitor::GetAddresses() const -> const std::map<Address, IpAddressInfo>& { return _Addresses; }

auto InterfaceMonitor::GetAddressInfo(const Address& address) const -> std::optional<IpAddressInfo> {
  auto iterator = _Addresses.find(address);
  if (iterator != _Addresses.end()) {
    return iterator->second;
  }
  return std::nullopt;
}

auto InterfaceMonitor::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> { co_return ErrorCode{}; }

auto InterfaceMonitor::DoWork() -> Omni::Fiber::Coroutine<void> {
  HANDLE addressNotifyHandle{nullptr};
  NotifyUnicastIpAddressChange(AF_UNSPEC, &OnUnicastIpAddressChangedCallback, this, FALSE, &addressNotifyHandle);
  _Addresses = QueryInterfaces();

  while (true) {
    auto [cancel, hasTask] = co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(), [] -> void {}),
        Omni::Fiber::SelectPair(_TaskQueue, [] -> void {}));

    if (hasTask) {
      while (!_TaskQueue.IsEmpty()) {
        auto task = _TaskQueue.PopFront();
        HandleAddressChangeEvent(task.Event);
        task.CompletionPromise.set_value();
      }
    }

    if (cancel) {
      break;
    }
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
    if (address.is_loopback() || address.is_unspecified()) {
      return std::nullopt;
    }
    return address;
  } else if (addr.si_family == AF_INET6) {
    auto address =
        boost::asio::ip::address_v6(std::bit_cast<boost::asio::ip::address_v6::bytes_type>(addr.Ipv6.sin6_addr));
    if (address.is_loopback() || address.is_unspecified() || address.is_link_local()) {
      return std::nullopt;
    }
    return address;
  } else {
    return std::nullopt;
  }
}

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

    auto [iterator, inserted] =
        _Addresses.try_emplace(address, IpAddressInfo{.PrefixLength = event.Row.OnLinkPrefixLength,
                                                      .InterfaceIndex = event.Row.InterfaceIndex});
    if (inserted) {
      BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
          << "Address " << FormatAddress(address) << "/" << static_cast<int>(iterator->second.PrefixLength)
          << " added on interface, " << iterator->second.InterfaceIndex;
      return true;
    } else {
      if (iterator->second.PrefixLength != event.Row.OnLinkPrefixLength ||
          iterator->second.InterfaceIndex != event.Row.InterfaceIndex) {
        iterator->second.PrefixLength = event.Row.OnLinkPrefixLength;
        iterator->second.InterfaceIndex = event.Row.InterfaceIndex;
        BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
            << "Address " << FormatAddress(address) << "/" << static_cast<int>(iterator->second.PrefixLength)
            << " updated on interface, " << iterator->second.InterfaceIndex;
        return true;
      }
    }
    return false;
  }
  case MibDeleteInstance: {
    auto count = _Addresses.erase(address);
    if (count > 0) {
      BOOST_LOG_SEV(_Logger, boost::log::trivial::info) << "Address " << FormatAddress(address) << " removed";
    }
    return count > 0;
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
    if (iterator->second.PrefixLength != event.Row.OnLinkPrefixLength ||
        iterator->second.InterfaceIndex != event.Row.InterfaceIndex) {
      iterator->second.PrefixLength = event.Row.OnLinkPrefixLength;
      iterator->second.InterfaceIndex = event.Row.InterfaceIndex;
      BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
          << "Address " << FormatAddress(address) << "/" << static_cast<int>(iterator->second.PrefixLength)
          << " updated on interface, " << iterator->second.InterfaceIndex;
      return true;
    }
    return false;
  }
  default:
    return false;
  }
}

auto InterfaceMonitor::QueryInterfaces() -> std::map<Address, IpAddressInfo> {
  std::map<Address, IpAddressInfo> result;

  ULONG flags = GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME |
                GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_ALL_INTERFACES;
  ULONG family = AF_UNSPEC;
  ULONG outBufLen = 15360;
  std::vector<BYTE> buffer(outBufLen);

  auto* pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
  ULONG dwRetVal = GetAdaptersAddresses(family, flags, nullptr, pAddresses, &outBufLen);
  if (dwRetVal == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(outBufLen);
    pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    dwRetVal = GetAdaptersAddresses(family, flags, nullptr, pAddresses, &outBufLen);
  }

  if (dwRetVal != NO_ERROR) {
    return result;
  }

  for (const auto* pCurr = pAddresses; pCurr != nullptr; pCurr = pCurr->Next) {
    for (const auto* pUnicast = pCurr->FirstUnicastAddress; pUnicast != nullptr; pUnicast = pUnicast->Next) {
      if (pUnicast->Address.lpSockaddr == nullptr) {
        continue;
      }
      auto addressOpt = ConvertSockaddrInet(*reinterpret_cast<const SOCKADDR_INET*>(pUnicast->Address.lpSockaddr));
      if (!addressOpt.has_value()) {
        continue;
      }
      const auto& address = addressOpt.value();
      auto [iterator, inserted] = result.try_emplace(
          address, IpAddressInfo{.PrefixLength = pUnicast->OnLinkPrefixLength, .InterfaceIndex = pCurr->IfIndex});
      if (inserted) {
        BOOST_LOG_SEV(_Logger, boost::log::trivial::info)
            << "Address " << FormatAddress(address) << "/" << static_cast<int>(iterator->second.PrefixLength)
            << " discovered on interface, " << iterator->second.InterfaceIndex;
        continue;
      } else {
        BOOST_LOG_SEV(_Logger, boost::log::trivial::warning)
            << "Address " << FormatAddress(address) << " already exists on different interface, " << pCurr->IfIndex
            << " vs " << iterator->second.InterfaceIndex;
      }
    }
  }

  return result;
}

void WINAPI InterfaceMonitor::OnUnicastIpAddressChangedCallback(PVOID context, PMIB_UNICASTIPADDRESS_ROW row,
                                                                MIB_NOTIFICATION_TYPE notificationType) {
  auto* self = static_cast<InterfaceMonitor*>(context);
  std::promise<void> promise;
  auto future = promise.get_future();
  self->_TaskQueue.Push(Task{
      .Event = AddressChangeEvent{.NotificationType = notificationType, .Row = *row},
      .CompletionPromise = std::move(promise),
  });
  future.wait();
}

} // namespace gh::windows::network
