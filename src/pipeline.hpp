#pragma once

#include <memory>
#include <queue>
#include <vector>

#include "endpoint.hpp"
#include "error-code.hpp"
#include "filter.hpp"
#include "flowcontrol.hpp"
#include "packet.hpp"
#include "scoped_flag.hpp"

namespace gh {

class Pipeline : public std::enable_shared_from_this<Pipeline> {
public:
  Pipeline(std::shared_ptr<EndpointInput> in, const std::vector<std::shared_ptr<Filter>>& filters,
           std::shared_ptr<EndpointOutput> out);
  ~Pipeline() { Stop(); }

  void Start();
  void Stop();
  void Pause();
  void Resume();

  std::size_t Size() const { return _Buffers.size(); }

private:
  bool IsCritical(const ErrorCode& ec);

  void Process(ScopedFlag&& write, Packet&& p);
  void ProcessQueue(ScopedFlag&& write);

  void ScheduleRead();
  void ScheduleRead(ScopedFlag&& read);
  void ScheduleWrite(ScopedFlag&& write, Packet&& p);

  enum State { kNone, kStarting, kRunning, kPaused, kStopped, kError } _State = kNone;
  bool _ReadPending = false;
  bool _WritePending = false;

  std::shared_ptr<EndpointInput> _In;
  std::shared_ptr<EndpointOutput> _Out;
  std::vector<std::shared_ptr<Filter>> _Filters;
  std::queue<Packet> _Buffers;
  FlowControl<Pipeline> _Fc;
};

} // namespace gh
