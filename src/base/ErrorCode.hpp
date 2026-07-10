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
  explicit AppErrorCategory() = default;
  virtual ~AppErrorCategory() = default;

  AppErrorCategory(const AppErrorCategory&) = delete;
  auto operator=(const AppErrorCategory&) -> AppErrorCategory& = delete;
  AppErrorCategory(AppErrorCategory&&) = delete;
  auto operator=(AppErrorCategory&&) -> AppErrorCategory& = delete;

  auto name() const noexcept -> const char* override { return "great-hole error"; }
  auto message(int code) const -> std::string override { return _Errs.at(code); }

  enum Codes {
    kNoError = 0,
    kEndOfStream = 1,
    kOperationAborted = 2,
    kIncorrectState = 3,
    kAlreadyStarted = 4,
    kForkExecError = 5,
    kInvalidPacketReserved = 6,
  };

private:
  static constexpr auto _Errs = std::to_array({
      "no error",
      "end of stream",
      "operation aborted",
      "incorrect state",
      "already_started",
      "fork_exec_error",
      "invalid_packet_reserved",
  });
};

class AppMinorErrorCategory : public ErrorCategory {
public:
  explicit AppMinorErrorCategory() = default;
  virtual ~AppMinorErrorCategory() = default;

  AppMinorErrorCategory(const AppMinorErrorCategory&) = delete;
  auto operator=(const AppMinorErrorCategory&) -> AppMinorErrorCategory& = delete;
  AppMinorErrorCategory(AppMinorErrorCategory&&) = delete;
  auto operator=(AppMinorErrorCategory&&) -> AppMinorErrorCategory& = delete;

  auto name() const noexcept -> const char* override { return "great-hole minor error"; }
  auto message(int code) const -> std::string override { return _Errs.at(code); }

  enum Codes {
    kNoError = 0,
    kSourceIpMismatch = 1,
    kInvalidPacketSize = 2,
    kInvalidPacketSession = 3,
    kUnsupportedPacket = 4,
  };

private:
  static constexpr auto _Errs = std::to_array({
      "no error",
      "source ip mismatch",
      "invalid_packet_size",
      "invalid_packet_session",
      "unsupported packet",
  });
};

extern const AppErrorCategory kAppError;
extern const AppMinorErrorCategory kAppMinorError;

} // namespace gh
