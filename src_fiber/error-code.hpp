#pragma once

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
    kIncorrectState = 1,
    kAlreadyStarted = 2,
    kForkExecError = 3,
    kInvalidPacketSize = 4,
    kInvalidPacketSession = 5,
    kInvalidPacketReserved = 6,
  };

private:
  static const std::string _Errs[];
};

extern AppErrorCategory kAppError;

} // namespace gh
