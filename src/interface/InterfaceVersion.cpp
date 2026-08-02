#include "Interface.hpp"

#include "Version.h"

namespace gh::Interface {

auto PlatformInterface::GetVersion() -> std::string {
  return kVersion;
}

} // namespace gh::Interface
