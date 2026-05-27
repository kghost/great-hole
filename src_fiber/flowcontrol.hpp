#pragma once

namespace gh {

template <typename FlowOp> class FlowControl {
public:
  explicit FlowControl(FlowOp* o) : _Op(o), _Congesting(false), LowWaterMark(100), HighWaterMark(500) {}

  // XXX: implement red(random early detection)

  void AfterRead() {
    if (!_Congesting && _Op->Size() > HighWaterMark) {
      _Congesting = true;
      _Op->Pause();
    }
  }

  void AfterWrite() {
    if (_Congesting && _Op->Size() < LowWaterMark) {
      _Congesting = false;
      _Op->Resume();
    }
  }

  int LowWaterMark;
  int HighWaterMark;

private:
  bool _Congesting;
  FlowOp* _Op;
};

} // namespace gh
