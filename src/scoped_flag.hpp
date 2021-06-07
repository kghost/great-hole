#pragma once

#include <boost/optional.hpp>
#include <functional>

class scoped_flag {
public:
	scoped_flag(bool & flag) : flag(flag) {
		assert(!this->flag.value().get());
		this->flag.value().get() = true;
	}
	~scoped_flag() {
		if (flag) {
			assert(flag.value().get());
			flag.value().get() = false;
		}
	}

	scoped_flag(scoped_flag &) = delete;
	scoped_flag operator=(scoped_flag &) = delete;
	scoped_flag(scoped_flag && that) { flag.swap(that.flag); }
	scoped_flag& operator=(scoped_flag && that) {
		if (flag) { assert(flag.value().get()); flag.value().get() = false; flag.reset(); }
		flag.swap(that.flag);
		return *this;
	}
private:
	boost::optional<std::reference_wrapper<bool>> flag;
};
