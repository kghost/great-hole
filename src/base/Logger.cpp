#include "Logger.hpp"

#include <iostream>
#include <string>
#include <unordered_map>

#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/console.hpp>

namespace expr = boost::log::expressions;

namespace gh::base {

namespace {

auto ToBoostSeverity(Interface::LogLevel level) -> boost::log::trivial::severity_level {
  switch (level) {
  case Interface::LogLevel::Trace:
    return boost::log::trivial::trace;
  case Interface::LogLevel::Debug:
    return boost::log::trivial::debug;
  case Interface::LogLevel::Info:
    return boost::log::trivial::info;
  case Interface::LogLevel::Warning:
    return boost::log::trivial::warning;
  case Interface::LogLevel::Error:
    return boost::log::trivial::error;
  case Interface::LogLevel::Fatal:
  default:
    return boost::log::trivial::fatal;
  }
}

} // namespace

LogConfiguration::LogConfiguration() { InitLoggerFormat(); }

LogConfiguration::~LogConfiguration() { boost::log::core::get()->reset_filter(); }

void LogConfiguration::InitLoggerFormat() {
  boost::log::add_common_attributes();
  boost::log::add_console_log(std::clog,
                              boost::log::keywords::format =
                                  (expr::stream << "[" << expr::attr<std::string>("Channel") << "] ["
                                                << boost::log::trivial::severity << "]: " << expr::smessage));
  UpdateFilter();
}

void LogConfiguration::UpdateFilter() {
  boost::log::core::get()->set_filter([this](const boost::log::attribute_value_set& attrs) -> bool {
    auto severityVal = boost::log::extract<boost::log::trivial::severity_level>("Severity", attrs);
    if (!severityVal) {
      return true;
    }

    Interface::LogLevel targetLevel = _DefaultLogLevel;
    auto channelVal = boost::log::extract<std::string>("Channel", attrs);

    if (channelVal) {
      auto iterator = _ComponentLogLevels.find(channelVal.get());
      if (iterator != _ComponentLogLevels.end()) {
        targetLevel = iterator->second;
      }
    }

    if (targetLevel == Interface::LogLevel::Off) {
      return false;
    }

    return severityVal.get() >= ToBoostSeverity(targetLevel);
  });
}

void LogConfiguration::SetLogLevel(Interface::LogLevel level) {
  _DefaultLogLevel = level;
  UpdateFilter();
}

void LogConfiguration::SetComponentLogLevel(std::string_view component, Interface::LogLevel level) {
  _ComponentLogLevels[std::string(component)] = level;
  UpdateFilter();
}

} // namespace gh::base
