#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "Endpoint.hpp"
#include "ErrorCode.hpp"
#include "Filter.hpp"
#include "Interface.hpp"
#include "Service.hpp"

namespace gh {

using TrafficStats = Interface::TrafficStats;

class Pipeline : public std::enable_shared_from_this<Pipeline>, public Service {
public:
  enum class Direction : uint8_t {
    Forward,
    Backward,
  };

  Pipeline(std::shared_ptr<Endpoint> ep1, const std::vector<std::shared_ptr<Filter>>& filters,
           std::shared_ptr<Endpoint> ep2);
  ~Pipeline() override {}

  Pipeline(const Pipeline&) = delete;
  auto operator=(const Pipeline&) -> Pipeline& = delete;
  Pipeline(Pipeline&&) = delete;
  auto operator=(Pipeline&&) -> Pipeline& = delete;

  auto GetName() -> std::string { return std::format("Pipeline[{}:{}]", _Ep1->GetName(), _Ep2->GetName()); }
  auto Start() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto Stop() -> Omni::Fiber::Coroutine<ErrorCode> override;

  auto GetTrafficStats() const -> TrafficStats { return _TrafficStats; }

private:
  auto IsCritical(const ErrorCode& err) -> bool;
  auto RunDirection(std::shared_ptr<Endpoint> input, std::shared_ptr<Endpoint> output, Direction direction)
      -> Omni::Fiber::Coroutine<void>;
  auto GetNameWithDirection(Direction direction) -> std::string {
    if (direction == Direction::Forward) {
      return std::format("Pipeline[F] {}->{}", _Ep1->GetName(), _Ep2->GetName());
    } else {
      return std::format("Pipeline[B] {}->{}", _Ep2->GetName(), _Ep1->GetName());
    }
  }

  std::shared_ptr<Endpoint> _Ep1;
  std::shared_ptr<Endpoint> _Ep2;
  std::vector<std::shared_ptr<Filter>> _Filters;
  Cancel _Stop;
  TrafficStats _TrafficStats;
  std::shared_ptr<Omni::Fiber::Fiber> _Fiber1;
  std::shared_ptr<Omni::Fiber::Fiber> _Fiber2;
};

} // namespace gh
