#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include <boost/log/sources/severity_channel_logger.hpp>
#include <boost/log/trivial.hpp>

#include "Interface.hpp"

namespace gh::base {

using ComponentLogger = boost::log::sources::severity_channel_logger_mt<boost::log::trivial::severity_level>;

class LogConfiguration {
public:
  explicit LogConfiguration();
  ~LogConfiguration();

  LogConfiguration(const LogConfiguration&) = delete;
  auto operator=(const LogConfiguration&) -> LogConfiguration& = delete;
  LogConfiguration(LogConfiguration&&) = delete;
  auto operator=(LogConfiguration&&) -> LogConfiguration& = delete;

  void InitLoggerFormat();
  void SetLogLevel(Interface::LogLevel level);
  void SetComponentLogLevel(std::string_view component, Interface::LogLevel level);

private:
  void UpdateFilter();

  Interface::LogLevel _DefaultLogLevel = Interface::LogLevel::Info;
  std::unordered_map<std::string, Interface::LogLevel> _ComponentLogLevels;
};

} // namespace gh::base
