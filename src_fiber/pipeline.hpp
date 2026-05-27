#pragma once

#include <memory>
#include <vector>

#include "Coroutine.hpp"
#include "Event.hpp"
#include "endpoint.hpp"
#include "error-code.hpp"
#include "filter.hpp"

namespace gh {

class Pipeline : public std::enable_shared_from_this<Pipeline> {
public:
  Pipeline(std::shared_ptr<EndpointInput> in, const std::vector<std::shared_ptr<Filter>>& filters,
           std::shared_ptr<EndpointOutput> out);
  ~Pipeline() {}

  Omni::Fiber::Coroutine<void> Start(Omni::Fiber::Event<>& stopSignal);

private:
  bool IsCritical(const ErrorCode& ec);

  std::shared_ptr<EndpointInput> _In;
  std::shared_ptr<EndpointOutput> _Out;
  std::vector<std::shared_ptr<Filter>> _Filters;
};

} // namespace gh
