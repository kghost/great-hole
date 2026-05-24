#include "logging.hpp"

#include <memory>
#include <queue>

#include <boost/asio.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/log/core/core.hpp>
#include <boost/log/core/record_view.hpp>
#include <boost/log/sinks/basic_sink_backend.hpp>
#include <boost/log/sinks/frontend_requirements.hpp>
#include <boost/log/sinks/sync_frontend.hpp>

#include "endpoint.hpp"

namespace gh {

class AsioLogBackend
    : public boost::log::sinks::basic_formatted_sink_backend<char, boost::log::sinks::synchronized_feeding> {
public:
  explicit AsioLogBackend(const std::shared_ptr<EndpointOutput>& out) : _Impl(new Detail(out)) {}

  void consume(const boost::log::record_view& rec, const string_type& log) { _Impl->Write(log); }

private:
  class Detail : public std::enable_shared_from_this<Detail> {
  public:
    explicit Detail(const std::shared_ptr<EndpointOutput>& out) : _Out(out) {}

    void Write(const std::string& log) {
      _Q.push(log + '\n');
      ScheduleWrite();
    }

  private:
    void ScheduleWrite() {
      if (!_WritePending && !_Q.empty()) {
        _WritePending = true;

        auto p = Packet{Buffer(_Q.front()), nullptr};
        _Out->AsyncWrite(std::move(p), [me = shared_from_this()](const ErrorCode& ec, std::size_t bytes_transferred) {
          boost::asio::detail::throw_error(ec, "write log");
          me->_WritePending = false;
          me->_Q.pop();
          me->ScheduleWrite();
        });
      }
    }

    bool _WritePending = false;
    std::shared_ptr<EndpointOutput> _Out;
    std::queue<std::string> _Q;
  };

  std::shared_ptr<Detail> _Impl;
};

void InitLog(std::shared_ptr<EndpointOutput> out) {
  using TextSink = boost::log::sinks::synchronous_sink<AsioLogBackend>;
  boost::log::core::get()->add_sink(boost::make_shared<TextSink>(boost::make_shared<AsioLogBackend>(out)));
}

} // namespace gh
