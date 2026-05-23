#ifndef LOGGING_H
#define LOGGING_H

#include <boost/asio.hpp>
#include <memory>

class endpoint_output;

void init_log(std::shared_ptr<endpoint_output> out);

#endif /* end of include guard: LOGGING_H */
