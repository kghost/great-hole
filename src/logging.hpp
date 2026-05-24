#pragma once

#include <memory>

namespace gh {

class EndpointOutput;

void InitLog(std::shared_ptr<EndpointOutput> out);

} // namespace gh
