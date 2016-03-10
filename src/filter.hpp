#ifndef FILTER_H
#define FILTER_H

#include <memory>

#include "packet.hpp"

class filter {
	public:
		virtual ~filter() = 0;
		virtual packet pipe(packet &&p) = 0;
};

class filter_bidirection {
	public:
		virtual ~filter_bidirection() = 0;
		virtual std::shared_ptr<filter> forward() = 0;
		virtual std::shared_ptr<filter> backward() = 0;
};

template<typename base>
class filter_symmetric : public filter, public filter_bidirection, public std::enable_shared_from_this<base> {
	public:
		virtual std::shared_ptr<filter> forward() { return std::enable_shared_from_this<base>::shared_from_this(); }
		virtual std::shared_ptr<filter> backward() { return std::enable_shared_from_this<base>::shared_from_this(); }
};

#endif /* end of include guard: FILTER_H */
