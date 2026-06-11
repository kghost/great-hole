#pragma once

#include <memory>
#include <vector>

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "Endpoint.hpp"
#include "ErrorCode.hpp"
#include "Filter.hpp"
#include "Service.hpp"

namespace Omni::Fiber {
class Fiber;
}

namespace gh {

class Pipeline : public std::enable_shared_from_this<Pipeline>, public Service {
public:
  Pipeline(std::shared_ptr<Endpoint> ep1, const std::vector<std::shared_ptr<Filter>>& filters,
           std::shared_ptr<Endpoint> ep2);
  ~Pipeline() override {}

  std::string GetName() { return std::format("Pipeline({:p})", static_cast<void*>(this)); }
  std::string GetNameWithDirection(bool direction) {
    return std::format("{} {}", GetName(), direction ? "1->2" : "2->1");
  }

  Omni::Fiber::Coroutine<ErrorCode> Start() override;
  Omni::Fiber::Coroutine<ErrorCode> Stop() override;

private:
  bool IsCritical(const ErrorCode& ec);
  Omni::Fiber::Coroutine<void> RunDirection(std::shared_ptr<Endpoint> in, std::shared_ptr<Endpoint> out,
                                            bool direction);

  std::shared_ptr<Endpoint> _Ep1;
  std::shared_ptr<Endpoint> _Ep2;
  std::vector<std::shared_ptr<Filter>> _Filters;
  Cancel _Stop;
  std::shared_ptr<Omni::Fiber::Fiber> _Fiber1;
  std::shared_ptr<Omni::Fiber::Fiber> _Fiber2;
};

} // namespace gh
