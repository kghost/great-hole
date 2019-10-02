#ifndef LOGGING_H
#define LOGGING_H

#include <memory>
#include <boost/asio.hpp>

class endpoint_output;

void init_log(std::shared_ptr<endpoint_output> out);

#endif /* end of include guard: LOGGING_H */
