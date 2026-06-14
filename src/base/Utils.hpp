#pragma once

#include <functional>

#include <boost/asio.hpp>

namespace gh {

template <typename T>
  requires(!std::is_pointer_v<T> && !std::is_reference_v<T>)
struct Less {
  using is_transparent = void;
  bool operator()(const T& lhs, const T& rhs) const { return &lhs < &rhs; }
  bool operator()(std::reference_wrapper<T> lhs, std::reference_wrapper<T> rhs) const {
    return &lhs.get() < &rhs.get();
  }
  bool operator()(std::reference_wrapper<T> lhs, const T& rhs) const { return &lhs.get() < &rhs; }
  bool operator()(const T& lhs, std::reference_wrapper<T> rhs) const { return &lhs < &rhs.get(); }
};

inline boost::asio::ip::address_v6 MapToV6(const boost::asio::ip::address& address) {
  if (address.is_v6()) {
    return address.to_v6();
  } else {
    return boost::asio::ip::make_address_v6(boost::asio::ip::v4_mapped, address.to_v4());
  }
}

inline std::function<bool(int)> SocketProtector;

} // namespace gh

