#ifndef ENDPOINT_H
#define ENDPOINT_H

#include <boost/system/error_code.hpp>

#include "packet.hpp"

typedef void (read_handler)(boost::system::error_code, packet&);
typedef void (write_handler)(boost::system::error_code, packet&);

class endpoint {
	public:
		virtual ~endpoint() = 0;
		virtual void async_read(std::function<read_handler>) = 0;
		virtual void async_write(packet &p, std::function<write_handler>) = 0;
};

#endif /* end of include guard: ENDPOINT_H */
