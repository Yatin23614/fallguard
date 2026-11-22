// Library-free float32 CNN inference (same loops as the STM32 build's
// stm32_model_cnn_f32.cpp). Weights come from model_weights_f32.h, generated
// by ml/export.py with BatchNorm folded in; export.py runs a NumPy copy of
// these loops and checks it against Keras before writing the header.
//
// Replaces TFLite-Micro: the tflm_esp32 build for the classic ESP32 has no
// ESP-NN kernels and needed ~1.07 s per window. ~0.43 M multiply-adds on the
// ESP32's single-precision FPU take a few ms.
// Speed: the project builds with -Os; this hot loop gets -O3, and weights are
// copied from flash (cache-missy) into internal RAM once at start-up.
#pragma GCC optimize("O3")

#include "model.h"

#include <Arduino.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "model_weights_f32.h"

#if MODEL_F32_AVAILABLE

namespace {
// RAM copies of the weights (fall back to the flash arrays if malloc fails).
template <size_t N>
const float* toRam(const float (&src)[N]) {
  float* p = static_cast<float*>(malloc(sizeof(src)));
  if (!p) return src;
  memcpy(p, src, sizeof(src));
  return p;
}
const float *c1w = kC1W, *c1b = kC1B, *c2w = kC2W, *c2b = kC2B, *c3w = kC3W, *c3b = kC3B;
const float *d1w = kD1W, *d1b = kD1B, *d2w = kD2W, *d2b = kD2B;

constexpr int kMax(int a, int b) { return a > b ? a : b; }
constexpr int kBufFloats =
    kMax(kMax(kWin * kC1Out, (kWin / 2) * kC2Out), (kWin / 4) * kC3Out);
float bufA[kBufFloats];
float bufB[kBufFloats];
bool ready = false;

// 'same' padding, stride 1, ReLU. w layout [cout][k][cin], x layout [t][c].
void convRelu(const float* x, int L, int cin, const float* w, const float* b,
              int k, int cout, float* y) {
  const int half = (k - 1) / 2;
  for (int t = 0; t < L; ++t) {
    for (int o = 0; o < cout; ++o) {
      float s = b[o];
      const float* wo = w + o * k * cin;
      for (int j = 0; j < k; ++j) {
        const int tt = t + j - half;
        if (tt < 0 || tt >= L) continue;
        const float* xt = x + tt * cin;
        const float* wj = wo + j * cin;
        for (int c = 0; c < cin; ++c) s += wj[c] * xt[c];
      }
      y[t * cout + o] = s > 0 ? s : 0;
    }
  }
}

void maxPool2(const float* x, int L, int c, float* y) {
  for (int t = 0; t < L / 2; ++t)
    for (int ch = 0; ch < c; ++ch) {
      const float a = x[2 * t * c + ch], b = x[(2 * t + 1) * c + ch];
      y[t * c + ch] = a > b ? a : b;
    }
}

// w layout [out][in]
void dense(const float* x, int in, const float* w, const float* b, int out,
           float* y, bool relu) {
  for (int o = 0; o < out; ++o) {
    float s = b[o];
    for (int i = 0; i < in; ++i) s += w[o * in + i] * x[i];
    y[o] = (relu && s < 0) ? 0 : s;
  }
}
}  // namespace

bool modelInit() {
  c1w = toRam(kC1W); c1b = toRam(kC1B); c2w = toRam(kC2W); c2b = toRam(kC2B);
  c3w = toRam(kC3W); c3b = toRam(kC3B);
  d1w = toRam(kD1W); d1b = toRam(kD1B); d2w = toRam(kD2W); d2b = toRam(kD2B);
  ready = true;
  Serial.printf("# model ok: float32 CNN (%d bytes of buffers)\n", (int)(2 * sizeof(bufA)));
  return true;
}

bool modelAvailable() { return ready; }

bool modelPredict(const float window[kWin][kChannels], float probs[kNumClasses]) {
  if (!ready) return false;
  for (int t = 0; t < kWin; ++t)
    for (int c = 0; c < kChannels; ++c)
      bufA[t * kChannels + c] = (window[t][c] - kNormMean[c]) / kNormStd[c];

  int L = kWin;
  convRelu(bufA, L, kChannels, c1w, c1b, kC1K, kC1Out, bufB);
  maxPool2(bufB, L, kC1Out, bufA);
  L /= 2;
  convRelu(bufA, L, kC1Out, c2w, c2b, kC2K, kC2Out, bufB);
  maxPool2(bufB, L, kC2Out, bufA);
  L /= 2;
  convRelu(bufA, L, kC2Out, c3w, c3b, kC3K, kC3Out, bufB);

  float gap[kC3Out];
  for (int o = 0; o < kC3Out; ++o) {
    float s = 0;
    for (int t = 0; t < L; ++t) s += bufB[t * kC3Out + o];
    gap[o] = s / L;
  }
  float hidden[kD1Out];
  dense(gap, kC3Out, d1w, d1b, kD1Out, hidden, true);
  float logits[kNumClasses];
  dense(hidden, kD1Out, d2w, d2b, kNumClasses, logits, false);

  float mx = logits[0], sum = 0;
  for (int k = 1; k < kNumClasses; ++k) mx = logits[k] > mx ? logits[k] : mx;
  for (int k = 0; k < kNumClasses; ++k) sum += probs[k] = expf(logits[k] - mx);
  for (int k = 0; k < kNumClasses; ++k) probs[k] /= sum;
  return true;
}

#else  // no weights exported yet

bool modelInit() {
  Serial.println("# no model: run ml/export.py. Using rule-based detection only.");
  return false;
}
bool modelAvailable() { return false; }
bool modelPredict(const float[kWin][kChannels], float[kNumClasses]) { return false; }

#endif
