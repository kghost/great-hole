#pragma once

#include <memory>

#include "packet.hpp"

namespace gh {

class Filter {
public:
  virtual ~Filter() = 0;
  virtual Packet Pipe(Packet&& p) = 0;
};

class FilterBidirection {
public:
  virtual ~FilterBidirection() = 0;
  virtual std::shared_ptr<Filter> Forward() = 0;
  virtual std::shared_ptr<Filter> Backward() = 0;
};

template <typename Base>
class FilterSymmetric : public Filter, public FilterBidirection, public std::enable_shared_from_this<Base> {
public:
  std::shared_ptr<Filter> Forward() override { return std::enable_shared_from_this<Base>::shared_from_this(); }
  std::shared_ptr<Filter> Backward() override { return std::enable_shared_from_this<Base>::shared_from_this(); }
};

} // namespace gh
