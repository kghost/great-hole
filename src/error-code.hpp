#pragma once

#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

namespace gh {
	typedef boost::system::error_code error_code;
	typedef boost::system::error_category error_category;
	typedef boost::system::error_condition error_condition;

	typedef boost::system::system_error system_error;

	using boost::system::system_category;
}

class app_error_category : public gh::error_category {
	public:
		virtual const char * name() const noexcept { return "great-hole error"; }
		virtual std::string message(int ev) const { return errs[ev]; }

		enum codes {
			incorrect_state = 1,
			already_started = 2,
			fork_exec_error = 3,
			invalid_packet_size = 4,
			invalid_packet_session = 5,
			invalid_packet_reserved = 6,
		};
	private:
		static const std::string errs[];
};

extern app_error_category app_error;
