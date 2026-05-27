#include "error-code.hpp"

namespace gh {

AppErrorCategory kAppError;

const std::string AppErrorCategory::_Errs[]{
    "end of stream",       "incorrect state",        "already_started",         "fork_exec_error",
    "invalid_packet_size", "invalid_packet_session", "invalid_packet_reserved",
};

} // namespace gh
