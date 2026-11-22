#pragma once
#include <stdint.h>

#include "imu.h"
#include "norm_params.h"

// Stages 1-3 of the pipeline, fed one sample at a time (no timing, no I/O,
// so the same logic can be mirrored in ml/evaluate.py).
//   Idle --impact--> WaitPost --CNN says fall--> Stillness --confirmed--> event
//                       \--CNN says ADL--> Cooldown <------ not confirmed --/
enum class DetState : uint8_t { Idle, WaitPost, Stillness, Cooldown };

struct Classification {
  float probs[kNumClasses];
  bool hasModel;
  bool fallLike;
};

struct FallEvent {
  Classification cls;
  float angleDeg;   // trunk orientation change, before vs after impact
  float stillFrac;  // fraction of "still" samples after impact
};

class Detector {
 public:
  // Returns true when a fall is confirmed; details in `ev`.
  bool push(const ImuSample& s, FallEvent& ev);
  // True once after each CNN run (for logging), with its result.
  bool takeClassification(Classification& out);
  DetState state() const { return st_; }
  void reset();

 private:
  static constexpr int kBuf = 256;
  const ImuSample& at(uint32_t i) const { return buf_[i % kBuf]; }
  void fillWindow(uint32_t peak);

  ImuSample buf_[kBuf];
  float window_[kWin][kChannels];
  uint32_t n_ = 0;
  DetState st_ = DetState::Idle;
  bool haveFF_ = false;
  uint32_t lastFF_ = 0, trig_ = 0, cooldownUntil_ = 0;
  // stillness stage
  float pre_[3] = {0, 0, 0}, sum_[3] = {0, 0, 0};
  int cnt_ = 0, still_ = 0;
  Classification cls_{};
  bool newCls_ = false;
};
