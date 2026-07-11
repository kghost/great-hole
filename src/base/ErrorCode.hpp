#pragma once

#include <array>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

#ifdef _WIN32
#include <Windows.h>
#endif

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

  enum Codes : uint8_t {
    kNoError = 0,
    kEndOfStream = 1,
    kOperationAborted = 2,
    kIncorrectState = 3,
    kAlreadyStarted = 4,
    kForkExecError = 5,
    kInvalidPacketReserved = 6,
  };

  static const AppErrorCategory kErrorCategory;

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

  enum Codes : uint8_t {
    kNoError = 0,
    kSourceIpMismatch = 1,
    kInvalidPacketSize = 2,
    kInvalidPacketSession = 3,
    kUnsupportedPacket = 4,
  };

  static const AppMinorErrorCategory kErrorCategory;

private:
  static constexpr auto _Errs = std::to_array({
      "no error",
      "source ip mismatch",
      "invalid_packet_size",
      "invalid_packet_session",
      "unsupported packet",
  });
};

template <typename ErrorCodes> struct CategoryOfCode;
template <> struct CategoryOfCode<AppErrorCategory::Codes> {
  using Category = AppErrorCategory;
};
template <> struct CategoryOfCode<AppMinorErrorCategory::Codes> {
  using Category = AppMinorErrorCategory;
};

auto Error(auto code) -> ErrorCode { return ErrorCode{code, CategoryOfCode<decltype(code)>::Category::kErrorCategory}; }

#ifndef _WIN32
inline auto SysError(int err) -> ErrorCode { return ErrorCode{err, system_category()}; }
#else
inline auto SysError(DWORD err) -> ErrorCode { return ErrorCode{static_cast<int>(err), system_category()}; }
#endif

} // namespace gh
