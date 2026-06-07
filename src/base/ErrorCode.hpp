#pragma once

#include <array>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

namespace gh {

using ErrorCode = boost::system::error_code;
using ErrorCategory = boost::system::error_category;
using ErrorCondition = boost::system::error_condition;
using SystemError = boost::system::system_error;

using boost::system::system_category;

class AppErrorCategory : public ErrorCategory {
public:
  const char* name() const noexcept override { return "great-hole error"; }
  std::string message(int ev) const override { return _Errs[ev]; }

  enum Codes {
    kNoError = 0,
    kEndOfStream = 1,
    kOperationAborted = 2,
    kIncorrectState = 3,
    kAlreadyStarted = 4,
    kForkExecError = 5,
    kInvalidPacketSize = 6,
    kInvalidPacketSession = 7,
    kInvalidPacketReserved = 8,
  };

private:
  static constexpr auto _Errs = std::to_array({
      "no error",
      "end of stream",
      "operation aborted",
      "incorrect state",
      "already_started",
      "fork_exec_error",
      "invalid_packet_size",
      "invalid_packet_session",
      "invalid_packet_reserved",
  });
};

class AppMinorErrorCategory : public ErrorCategory {
public:
  const char* name() const noexcept override { return "great-hole minor error"; }
  std::string message(int ev) const override { return _Errs[ev]; }

  enum Codes {
    kNoError = 0,
    kSourceIpMismatch = 1,
  };

private:
  static constexpr auto _Errs = std::to_array({
      "no error",
      "source ip mismatch",
  });
};

extern AppErrorCategory kAppError;
extern AppMinorErrorCategory kAppMinorError;

} // namespace gh
