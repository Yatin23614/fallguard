#include "detector.h"

#include <math.h>

#include "config.h"
#include "model.h"

namespace {
inline float accMag(const ImuSample& s) {
  return sqrtf(s.ax * s.ax + s.ay * s.ay + s.az * s.az);
}
inline float gyroMag(const ImuSample& s) {
  return sqrtf(s.gx * s.gx + s.gy * s.gy + s.gz * s.gz);
}
}  // namespace

void Detector::reset() {
  st_ = DetState::Idle;
  haveFF_ = false;
}

bool Detector::takeClassification(Classification& out) {
  if (!newCls_) return false;
  out = cls_;
  newCls_ = false;
  return true;
}

// WIN samples with `peak` at index kPre, edge-padded (matches preprocess.py).
void Detector::fillWindow(uint32_t peak) {
  const int64_t oldest = (int64_t)n_ > kBuf ? (int64_t)n_ - kBuf : 0;
  const int64_t newest = (int64_t)n_ - 1;
  const int64_t start = (int64_t)peak - kPre;
  for (int t = 0; t < kWin; ++t) {
    int64_t idx = start + t;
    if (idx < oldest) idx = oldest;
    if (idx > newest) idx = newest;
    const ImuSample& s = at((uint32_t)idx);
    window_[t][0] = s.ax; window_[t][1] = s.ay; window_[t][2] = s.az;
    window_[t][3] = s.gx; window_[t][4] = s.gy; window_[t][5] = s.gz;
  }
}

bool Detector::push(const ImuSample& s, FallEvent& ev) {
  const uint32_t i = n_;
  buf_[i % kBuf] = s;
  ++n_;
  const float mag = accMag(s);

  switch (st_) {
    case DetState::Idle: {
      if (mag < FREEFALL_G) { haveFF_ = true; lastFF_ = i; }
      const bool afterFF = haveFF_ && (i - lastFF_) <= (uint32_t)FREEFALL_WINDOW;
      if ((mag > IMPACT_G && afterFF) || mag > IMPACT_ONLY_G) {
        trig_ = i;
        st_ = DetState::WaitPost;
      }
      break;
    }

    case DetState::WaitPost: {
      // Wait until the full post-impact half of the window has arrived.
      if (i < trig_ + 10 + (kWin - kPre)) break;
      uint32_t lo = trig_ >= 5 ? trig_ - 5 : 0, peak = trig_;
      float best = -1;
      for (uint32_t j = lo; j <= trig_ + 10; ++j) {
        float m = accMag(at(j));
        if (m > best) { best = m; peak = j; }
      }
      fillWindow(peak);

      cls_.hasModel = modelAvailable();
      if (cls_.hasModel && modelPredict(window_, cls_.probs)) {
        cls_.fallLike = cls_.probs[kFallIdx] >= FALL_PROB_MIN;
      } else {  // no model: rely on trigger + post-impact rules
        for (int k = 0; k < kNumClasses; ++k) cls_.probs[k] = k == kFallIdx;
        cls_.fallLike = true;
      }
      newCls_ = true;

      if (cls_.fallLike) {
        // Orientation before the fall: first 0.5 s of the window.
        pre_[0] = pre_[1] = pre_[2] = 0;
        for (int t = 0; t < 25; ++t)
          for (int c = 0; c < 3; ++c) pre_[c] += window_[t][c] / 25.0f;
        sum_[0] = sum_[1] = sum_[2] = 0;
        cnt_ = still_ = 0;
        st_ = DetState::Stillness;
      } else {
        cooldownUntil_ = i + COOLDOWN_SAMPLES;
        st_ = DetState::Cooldown;
      }
      break;
    }

    case DetState::Stillness: {
      sum_[0] += s.ax; sum_[1] += s.ay; sum_[2] += s.az;
      if (fabsf(mag - 1.0f) < STILL_ACC_TOL_G && gyroMag(s) < STILL_GYRO_DPS) ++still_;
      if (++cnt_ < STILL_SAMPLES) break;

      const float post[3] = {sum_[0] / cnt_, sum_[1] / cnt_, sum_[2] / cnt_};
      const float dot = pre_[0] * post[0] + pre_[1] * post[1] + pre_[2] * post[2];
      const float na = sqrtf(pre_[0] * pre_[0] + pre_[1] * pre_[1] + pre_[2] * pre_[2]);
      const float nb = sqrtf(post[0] * post[0] + post[1] * post[1] + post[2] * post[2]);
      float cosv = (na > 1e-3f && nb > 1e-3f) ? dot / (na * nb) : 1.0f;
      cosv = cosv > 1 ? 1 : (cosv < -1 ? -1 : cosv);

      ev.cls = cls_;
      ev.angleDeg = acosf(cosv) * 57.29578f;
      ev.stillFrac = (float)still_ / cnt_;
      cooldownUntil_ = i + COOLDOWN_SAMPLES;
      st_ = DetState::Cooldown;
      // Lying after a fall changes trunk orientation; a fall that ends seated
      // is caught by the person staying still.
      return ev.angleDeg >= ORIENT_CHANGE_DEG || ev.stillFrac >= STILL_FRAC_MIN;
    }

    case DetState::Cooldown:
      if (i >= cooldownUntil_) reset();
      break;
  }
  return false;
}
