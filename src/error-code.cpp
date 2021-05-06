#include "config.h"

#include "error-code.hpp"

app_error_category app_error;

const std::string app_error_category::errs[] {
	"",
	"incorrect state",
	"already_started",
	"fork_exec_error",
	"invalid_packet_size",
	"invalid_packet_session",
	"invalid_packet_reserved",
};
