#pragma once
// Pins and tuning. Detector thresholds mirror ml/config.py -- keep in sync.

// ---- Pins ------------------------------------------------------------------
// Matches the current wiring (found with env:diag). The textbook ESP32
// default is SDA=21/SCL=22; swap back if you rewire to that.
constexpr int PIN_SDA = 22;
constexpr int PIN_SCL = 21;
constexpr int PIN_MPU_INT = 19;     // wired for future motion-wake; unused in v1
constexpr int PIN_BUTTON = 4;       // to GND, internal pull-up
constexpr int PIN_BUZZER = 25;      // active buzzer (HIGH = sound)
constexpr int PIN_LED = 2;          // on-board LED
constexpr int PIN_BATT_ADC = -1;    // set to e.g. 35 if you add a 1:2 divider

// ---- Sampling --------------------------------------------------------------
constexpr int SAMPLE_HZ = 50;
constexpr unsigned long SAMPLE_PERIOD_US = 1000000UL / SAMPLE_HZ;

// ---- Stage 1: impact trigger ------------------------------------------------
constexpr float FREEFALL_G = 0.6f;
constexpr float IMPACT_G = 2.5f;
constexpr float IMPACT_ONLY_G = 3.0f;
constexpr int FREEFALL_WINDOW = 50;     // samples (1 s)

// ---- Stage 2: CNN ----------------------------------------------------------
constexpr float FALL_PROB_MIN = 0.6f;

// ---- Stage 3: post-impact check -------------------------------------------
constexpr int STILL_SAMPLES = 100;          // 2 s observed after the window
constexpr float STILL_ACC_TOL_G = 0.15f;    // | |a| - 1 g | below this ...
constexpr float STILL_GYRO_DPS = 20.0f;     // ... and |gyro| below this = still
constexpr float STILL_FRAC_MIN = 0.7f;      // fraction of still samples
constexpr float ORIENT_CHANGE_DEG = 45.0f;  // trunk tilt vs. before the fall
constexpr int COOLDOWN_SAMPLES = 25;

// ---- Alerting --------------------------------------------------------------
constexpr unsigned long PRE_ALERT_MS = 15000;   // time to cancel a false alarm
constexpr unsigned long LONG_PRESS_MS = 3000;   // manual SOS
constexpr unsigned long ALERT_RESEND_MS = 5000; // re-notify until acknowledged
constexpr unsigned long STATUS_PERIOD_MS = 60000;
constexpr const char* BLE_DEVICE_NAME = "FallGuard";
