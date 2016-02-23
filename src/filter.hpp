#ifndef FILTER_H
#define FILTER_H

#include "packet.hpp"

class filter {
	public:
		virtual ~filter() = 0;
		virtual packet pipe(packet &p) = 0;
};

#endif /* end of include guard: FILTER_H */
