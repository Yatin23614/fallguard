#pragma once
#include "norm_params.h"

// Float32 CNN with weights from model_weights_f32.h. Returns false if no model
// was exported yet (the detector then falls back to trigger + stillness rules).
bool modelInit();
bool modelAvailable();

// window: raw samples (g, deg/s) in device frame; normalisation happens
// inside, exactly as in ml/export.py (c_forward).
bool modelPredict(const float window[kWin][kChannels], float probs[kNumClasses]);
