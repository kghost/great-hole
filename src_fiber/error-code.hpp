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
    kEndOfStream = 0,
    kOperationAborted = 1,
    kIncorrectState = 2,
    kAlreadyStarted = 3,
    kForkExecError = 4,
    kInvalidPacketSize = 5,
    kInvalidPacketSession = 6,
    kInvalidPacketReserved = 7,
  };

private:
  static constexpr auto _Errs = std::to_array({
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

extern AppErrorCategory kAppError;

} // namespace gh
