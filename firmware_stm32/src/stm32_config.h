#pragma once
// STM32 Nucleo-F401RE pins and tuning.
// Detector thresholds mirror ml/config.py and firmware/src/config.h -- keep in sync.
#include <Arduino.h>

// ---- Pins (Arduino-header name in brackets) ---------------------------------
#define PIN_SDA        PB9    // [D14] I2C1 SDA -> MPU-6050 SDA
#define PIN_SCL        PB8    // [D15] I2C1 SCL -> MPU-6050 SCL
#define PIN_MPU_INT    PA8    // [D7]  MPU-6050 INT (reserved, unused in v1)
#define PIN_BLE_RX     PA10   // [D2]  USART1 RX  <- HM-10 TXD
#define PIN_BLE_TX     PA9    // [D8]  USART1 TX  -> HM-10 RXD
#define PIN_BUTTON     PC13   // on-board blue USER button (B1), active LOW
#define PIN_BUZZER     PB4    // [D5]  active buzzer (+)
#define PIN_LED        PA5    // [D13] on-board green LED LD2
constexpr int PIN_BATT_ADC = -1;      // e.g. PA0 [A0] with a 1:2 battery divider

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
constexpr int STILL_SAMPLES = 100;
constexpr float STILL_ACC_TOL_G = 0.15f;
constexpr float STILL_GYRO_DPS = 20.0f;
constexpr float STILL_FRAC_MIN = 0.7f;
constexpr float ORIENT_CHANGE_DEG = 45.0f;
constexpr int COOLDOWN_SAMPLES = 25;

// ---- Alerting --------------------------------------------------------------
constexpr unsigned long PRE_ALERT_MS = 15000;
constexpr unsigned long LONG_PRESS_MS = 3000;
constexpr unsigned long ALERT_RESEND_MS = 5000;
constexpr unsigned long STATUS_PERIOD_MS = 60000;
constexpr const char* BLE_DEVICE_NAME = "FallGuard";   // max 12 chars on HM-10
constexpr unsigned long HM10_BAUD = 9600;              // HM-10 factory default
// Genuine HM-10 (CC2541) takes bare "AT..." commands. Many clones (BT05,
// JDY-08, MLT-BT05) need "\r\n" after each command -- set true for those.
constexpr bool HM10_AT_CRLF = false;
