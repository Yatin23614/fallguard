# FallGuard: complete project documentation

FallGuard is a waist-worn wearable that detects falls and raises an alert over Bluetooth Low Energy (BLE) and Wi-Fi. A microcontroller reads a 6-axis motion sensor 50 times a second. A cheap rule spots impacts, a small neural network (1D-CNN) decides whether each impact was a fall or just sitting or lying down, and a post-impact check confirms that the wearer stayed down. The wearer then has 15 seconds to cancel before the alert goes out.

This document covers the whole project: hardware, signal conventions, the detection algorithm, the neural network, the Python training pipeline, both firmware builds, every communication protocol, the receivers, current results, workflows, and how to change things safely.

> This is a prototype, not a certified medical device.

---

## Contents

1. [System at a glance](#1-system-at-a-glance)
2. [Repository map](#2-repository-map)
3. [Hardware and wiring](#3-hardware-and-wiring)
4. [Signal conventions](#4-signal-conventions)
5. [The detection pipeline](#5-the-detection-pipeline)
6. [The alert state machine and the button](#6-the-alert-state-machine-and-the-button)
7. [The neural network](#7-the-neural-network)
8. [The ML pipeline (Python)](#8-the-ml-pipeline-python)
9. [ESP32 firmware](#9-esp32-firmware)
10. [STM32 firmware](#10-stm32-firmware)
11. [Communication protocols](#11-communication-protocols)
12. [Receivers: laptop script and web dashboard](#12-receivers-laptop-script-and-web-dashboard)
13. [Current results](#13-current-results)
14. [End-to-end workflow](#14-end-to-end-workflow)
15. [Changing things safely](#15-changing-things-safely)
16. [Troubleshooting](#16-troubleshooting)
17. [Limitations and known gaps](#17-limitations-and-known-gaps)
18. [Glossary](#18-glossary)

---

## 1. System at a glance

```
               ┌──────────────────── wearable (ESP32 or STM32) ─────────────────────┐
 MPU-6050 ──►  │ 50 Hz sampling ─► Stage 1: impact trigger                          │
 (I²C)         │                   free-fall < 0.6 g then > 2.5 g, or any > 3 g     │
               │                 ─► Stage 2: 1D-CNN on a 2.56 s window around peak  │
               │                   → fall / sit / lie / other                       │
               │                 ─► Stage 3: 2 s post-impact check                  │
               │                   trunk tilt ≥ 45° or still ≥ 70% of the time      │
               │                 ─► 15 s pre-alert (beeping; short press cancels)   │
               │                 ─► ALERT, re-sent every 5 s until acknowledged     │
               │ Hold button 3 s at any time = manual SOS                           │
               └───────┬───────────────────────┬────────────────────────────────────┘
                       │ BLE (GATT / HM-10)    │ Wi-Fi HTTP (ESP32 only)
                       ▼                       ▼
         receiver/ble_receiver.py     http://fallguard.local/  (dashboard served
         web/index.html (Web BLE)     by the device; polls /live, POST /ack)
         nRF Connect on a phone
```

Two firmware builds share one Python training pipeline and one detection algorithm:

| | ESP32 build | STM32 build |
|---|---|---|
| Folder | [firmware/](../firmware/) | [firmware_stm32/](../firmware_stm32/) (files prefixed `stm32_`) |
| Board | ESP32 DevKit v1 (`esp32dev`) | STM32 Nucleo-F401RE |
| BLE | Built-in radio, custom GATT service with 4 characteristics | External HM-10 BLE-UART module (one characteristic) |
| Wi-Fi dashboard | Yes, served by the device | No |
| Live motion stream and impact events | Yes | No (alerts and status only) |
| CNN runtime | Library-free float32 engine ([model.cpp](../firmware/src/model.cpp)) | Same float32 engine ([stm32_model_cnn_f32.cpp](../firmware_stm32/src/stm32_model_cnn_f32.cpp)) |
| Upload and serial | USB-UART (CP2102/CH340) | On-board ST-LINK virtual COM port |

---

## 2. Repository map

```
Fall_detection_project/
├── README.md                     quick-start guide
├── docs/DOCUMENTATION.md         this file
├── .gitignore                    ignores data/, models/, .pio/, logs, wifi_secrets.h, web_index.h
│
├── ml/                           Python: data → trained model → firmware headers
│   ├── config.py                 every shared constant (window size, classes, thresholds, paths)
│   ├── common.py                 load windows, subject split, normalisation, metrics report
│   ├── download_sisfall.py       unpacks the SisFall zip into data/SisFall_dataset
│   ├── preprocess.py             SisFall + own CSVs → data/processed/windows.npz
│   ├── record.py                 records labelled trials from the logger firmware
│   ├── train.py                  builds and trains the CNN → models/*.keras, norm.json, splits.json
│   ├── export.py                 Keras → int8 .tflite + float32 C headers for both boards
│   ├── evaluate.py               float vs int8 metrics + simulated false alarms per hour
│   ├── parity_check.py           compares the board's CNN output with Keras over serial
│   └── requirements.txt          tensorflow>=2.15, numpy, scipy, pyserial, bleak
│
├── firmware/                     ESP32 (PlatformIO, Arduino framework)
│   ├── platformio.ini            envs: logger, diag, detector
│   ├── embed_web.py              pre-build: gzips web/index.html → src/web_index.h
│   └── src/
│       ├── main.cpp              setup()/loop() → appSetup()/appLoop()
│       ├── app.h                 the appSetup/appLoop interface
│       ├── app_logger.cpp        env:logger   – streams CSV at 50 Hz
│       ├── app_diag.cpp          env:diag     – finds which GPIOs the sensor is on
│       ├── app_detector.cpp      env:detector – detection, alerts, button, buzzer, parity mode
│       ├── config.h              pins and all tuning constants
│       ├── imu.h / imu.cpp       MPU-6050 setup and reading (g, °/s)
│       ├── detector.h / .cpp     Stages 1–3 as a sample-by-sample state machine
│       ├── model.h / model.cpp   float32 CNN inference engine
│       ├── model_weights_f32.h   GENERATED by export.py – BN-folded weights
│       ├── norm_params.h         GENERATED by export.py – window size, normalisation, class names
│       ├── ble_alert.h / .cpp    NimBLE GATT server
│       ├── wifi_link.h / .cpp    Wi-Fi station, mDNS, HTTP server, JSON API
│       ├── wifi_secrets.example.h  template for wifi_secrets.h (git-ignored)
│       └── web_index.h           GENERATED at build time (git-ignored)
│
├── firmware_stm32/               STM32 Nucleo-F401RE port
│   ├── platformio.ini            envs: stm32_logger, stm32_detector
│   └── src/                      same structure, prefixed stm32_; BLE via stm32_ble_hm10.cpp
│
├── receiver/ble_receiver.py      laptop BLE listener (ESP32 or --hm10), auto-ack
├── web/index.html                single-file dashboard (Web Bluetooth, Wi-Fi polling, demo mode)
│
├── data/                         (git-ignored) SisFall_dataset/, own/<subject>/*.csv, processed/windows.npz
└── models/                       (git-ignored) pretrained.keras, final.keras, model_int8.tflite,
                                  norm.json, splits.json
```

Generated files are never edited by hand. `model_weights_f32.h` and `norm_params.h` (and their `stm32_` copies) are committed so the firmware builds without running Python first. `web_index.h` is rebuilt on every ESP32 detector build.

---

## 3. Hardware and wiring

### Bill of materials

| Part | Purpose |
|---|---|
| ESP32 DevKit v1, **or** STM32 Nucleo-F401RE + HM-10 BLE module | Main controller |
| MPU-6050 breakout (GY-521) | 3-axis accelerometer + 3-axis gyroscope over I²C |
| Momentary push button | Cancel / "I'm OK" (short press), manual SOS (hold 3 s). On the Nucleo, the on-board blue button is used. |
| Active buzzer | Audible pre-alert and alert |
| LED | On-board LED is used |
| Optional: 1:2 resistor divider | Battery voltage sensing |

### ESP32 wiring (pins in [config.h](../firmware/src/config.h))

| Part pin | ESP32 pin | Note |
|---|---|---|
| MPU-6050 VCC / GND | 3V3 / GND | |
| MPU-6050 SDA | **GPIO22** | Swapped from the usual ESP32 default (SDA=21) to match the current wiring, which `env:diag` found |
| MPU-6050 SCL | **GPIO21** | |
| MPU-6050 INT | GPIO19 | Wired for future motion-wake; unused |
| MPU-6050 AD0 | GND or open | Address 0x68 (firmware also tries 0x69) |
| Button | GPIO4 ↔ GND | Internal pull-up, active LOW |
| Buzzer + | GPIO25 | HIGH = sound |
| LED | GPIO2 | On-board |
| Battery divider midpoint | GPIO35 (optional) | Set `PIN_BATT_ADC = 35`; default `-1` = not measured |

### STM32 Nucleo-F401RE wiring (pins in [stm32_config.h](../firmware_stm32/src/stm32_config.h))

| Part pin | Nucleo pin (header / MCU) |
|---|---|
| MPU-6050 SDA / SCL | D14 / PB9, D15 / PB8 (I2C1) |
| MPU-6050 INT | D7 / PA8 (reserved) |
| HM-10 TXD → | D2 / PA10 (USART1 RX) |
| HM-10 RXD ← | D8 / PA9 (USART1 TX) |
| HM-10 VCC | 5V for most breakouts (3V3 for a bare module) |
| Button | On-board B1 (PC13), no wiring |
| Buzzer + | D5 / PB4 |
| LED | On-board LD2 (D13 / PA5) |
| Battery divider midpoint | A0 / PA0 (optional) |

D0/D1 are used by the ST-LINK virtual COM port (USART2), so leave them free.

### Mounting

Clip the device firmly to the belt, **always in the same orientation**. The CNN learned what gravity looks like in that orientation. The trained normalisation shows gravity mostly on the −Y axis (mean `ay ≈ −0.72 g`), so the expected mounting has the board's −Y axis pointing down when standing. The dashboard's tilt readout makes the same assumption.

---

## 4. Signal conventions

These conventions are shared by the firmware, the recorded CSVs and the Python pipeline. Breaking any of them silently breaks the model.

| Item | Value |
|---|---|
| Sample rate | 50 Hz (`SAMPLE_HZ`, `FS`), one sample every 20 ms |
| Channels, in this order | `ax, ay, az` in **g**, `gx, gy, gz` in **°/s** |
| Accelerometer range | ±16 g (falls can exceed 8 g) |
| Gyroscope range | ±2000 °/s |
| Sensor low-pass filter | 21 Hz on-chip DLPF (anti-aliasing for 50 Hz) |
| Window | 128 samples = 2.56 s (`WIN`, `kWin`) |
| Impact position in the window | Index 64 (`PRE`, `kPre`): 1.28 s before, 1.28 s after |
| Window edge handling | Edge padding: missing samples repeat the first/last available sample |
| Classes, in this order | `fall` (index 0), `sit`, `lie`, `other` |
| Normalisation | Per channel `(x − mean) / std`, computed from training data, shipped in `norm_params.h` |

The MPU-6050 library returns m/s² and rad/s. [imu.cpp](../firmware/src/imu.cpp) converts to g (÷ 9.80665) and °/s (× 57.29578).

---

## 5. The detection pipeline

Implemented in [detector.cpp](../firmware/src/detector.cpp) (identical copy in `stm32_fall_detector.cpp`). The `Detector` class is pure logic: it receives one sample at a time through `push()` and has no timers or I/O, so [evaluate.py](../ml/evaluate.py) can mirror it in Python. It keeps the last 256 samples (5.12 s) in a ring buffer.

```
          impact                    CNN says fall
  Idle ───────────► WaitPost ─────────────────────► Stillness ── 100 samples ──► decision
   ▲                   │ CNN says sit/lie/other                                     │
   │                   ▼                                                            │
   └──── 25 samples ── Cooldown ◄───────────────────────────────────────────────────┘
                                         (confirmed → FallEvent returned, or rejected)
```

The state number (`Idle=0, WaitPost=1, Stillness=2, Cooldown=3`) is streamed to the dashboard, which lights up the matching stage.

### Stage 1: impact trigger (state `Idle`)

For each sample, `|a| = √(ax² + ay² + az²)`.

- If `|a| < 0.6 g` (`FREEFALL_G`), remember this as the last free-fall sample.
- Trigger if `|a| > 2.5 g` (`IMPACT_G`) **and** free-fall happened within the last 50 samples (1 s, `FREEFALL_WINDOW`).
- Or trigger if `|a| > 3.0 g` (`IMPACT_ONLY_G`), with or without free-fall.

This rule is cheap and deliberately sensitive. Sitting down hard also triggers it, which is why Stage 2 exists.

### Stage 2: CNN classification (state `WaitPost`)

1. Wait until 74 samples after the trigger (10 to find the peak + 64 for the second half of the window), about 1.48 s.
2. Find the true peak: the largest `|a|` from 5 samples before to 10 samples after the trigger.
3. Build the 128-sample window with the peak at index 64 (`fillWindow`, edge-padded exactly like `window_at()` in [preprocess.py](../ml/preprocess.py)).
4. Run the CNN. The impact is "fall-like" if `p(fall) ≥ 0.6` (`FALL_PROB_MIN`).
5. If no model was exported, the detector treats every impact as fall-like and relies on Stage 3 alone. It logs a warning at boot.

Fall-like → `Stillness`. Otherwise → `Cooldown`.

### Stage 3: post-impact check (state `Stillness`)

Before entering this stage, the detector saves the pre-fall orientation: the mean accelerometer vector over the first 0.5 s (25 samples) of the window. Then it observes the next 100 samples (2 s):

- A sample is **still** if `| |a| − 1 g | < 0.15 g` and `|gyro| < 20 °/s`.
- The mean accelerometer vector over these 2 s is the post-fall orientation.
- **Angle** = angle between the pre- and post-fall gravity vectors (degrees).
- **Still fraction** = still samples / 100.

The fall is **confirmed** if `angle ≥ 45°` **or** `still fraction ≥ 0.7`. Lying on the floor changes trunk orientation; a fall that ends seated is caught by the person staying still. Either way the detector goes to `Cooldown` and, if confirmed, returns a `FallEvent { classification, angleDeg, stillFrac }`.

### Cooldown

25 samples (0.5 s), then back to `Idle`. This stops one impact from triggering twice.

### Timing from impact to alert

| Step | Duration |
|---|---|
| Collect the second half of the window | ≈ 1.5 s |
| CNN inference | a few ms |
| Post-impact check | 2.0 s |
| Pre-alert (cancel period) | 15 s |
| **Total** | **≈ 18.5 s** after impact |

---

## 6. The alert state machine and the button

Implemented in [app_detector.cpp](../firmware/src/app_detector.cpp) (and `stm32_detector_app.cpp`).

```
            fall confirmed (only from Idle)        15 s elapsed
   Idle ───────────────────────────────► PreAlert ──────────────► Alerted ── re-sent every 5 s
    ▲  ▲                                   │                       │  │
    │  └───────── short press ("cancel") ──┘                       │  │
    │                                                              │  │
    └──────── short press ("cancel") or ack from BLE/Wi-Fi ("acked") ─┘  │
                                                                        │
   Hold button 3 s in Idle or PreAlert ─────────────────────────────────┘  (type "sos")
```

| State | Buzzer | LED |
|---|---|---|
| Idle | off | 50 ms blink every 3 s (heartbeat) |
| PreAlert | 150 ms beep every second | follows buzzer |
| Alerted | 500 ms on / 1.5 s off | follows buzzer |

Button handling (`pollButton`): 30 ms debounce. A release before 3 s is a **short press**; holding for 3 s (`LONG_PRESS_MS`) fires a **long press** once, while still held, and the release then does nothing.

Rules:
- A long press in Idle or PreAlert raises an **SOS** alert immediately (it replaces a pending fall). It is ignored while an alert is already active.
- A short press does nothing in Idle. In PreAlert or Alerted it clears the alert (`"cancel"`).
- An acknowledgement (BLE write or `POST /ack`) clears an active alert (`"acked"`). Both ack flags are consumed each loop, so a stale ack cannot clear a later alert.
- A fall confirmed while not Idle is logged but ignored.
- On clear, the alert characteristic is overwritten with `{"id":N,"type":"cancel"|"acked"}`, so a receiver that connects later doesn't think the old alert is still active.
- Status JSON is sent every 60 s and on every state change (ESP32; the STM32 sends it only every 60 s).

### Serial parity mode

The detector firmware also listens on serial for 128 lines of `X,ax,ay,az,gx,gy,gz` (raw g and °/s). It normalises them, runs the CNN exactly as it does live, and replies `P,p_fall,p_sit,p_lie,p_other,microseconds`. [parity_check.py](../ml/parity_check.py) uses this.

---

## 7. The neural network

Defined in `build_model()` in [train.py](../ml/train.py). It is a 1D convolution over time, written with Conv2D layers using `(k,1)` kernels on a `(128, 1, 6)` input. The maths is the same as Conv1D, but it converts to TFLite ops that every TFLite-Micro build supports (still relevant for the int8 reference model).

| # | Layer | Output shape | Parameters |
|---|---|---|---|
| | Input (normalised window) | 128 × 6 | |
| 1 | Conv k=5, 16 filters, no bias → BatchNorm → ReLU | 128 × 16 | 480 + 64 |
| | MaxPool 2 | 64 × 16 | |
| 2 | Conv k=5, 32 filters, no bias → BatchNorm → ReLU | 64 × 32 | 2,560 + 128 |
| | MaxPool 2 | 32 × 32 | |
| 3 | Conv k=3, 64 filters, no bias → BatchNorm → ReLU | 32 × 64 | 6,144 + 256 |
| | Global average over time | 64 | |
| 4 | Dense 32, ReLU (Dropout 0.3 in training only) | 32 | 2,080 |
| 5 | Dense 4, softmax | 4 | 132 |
| | **Total** | | **11,844** |

All convolutions use "same" padding and stride 1. Inference costs about 0.43 million multiply-adds.

### Training choices

- **Loss:** sparse categorical cross-entropy, Adam optimiser, batch 64.
- **Class weights:** inverse frequency, with the fall weight multiplied by 1.5, because missing a fall is worse than a false alarm.
- **Early stopping:** patience 8 epochs on validation loss, restoring the best weights.
- **Augmentation:** each trial produces 5 windows with the peak shifted by −12, −6, 0, +6, +12 samples, so the model tolerates imprecise peak alignment. "Other" trials longer than 256 samples also get 3 random non-peak windows (walking, stairs) so the model sees ordinary motion.
- **Two phases:** pre-train on SisFall (lr 1e-3, up to 40 epochs), then fine-tune on your own recordings mixed with up to 2× as many SisFall windows (lr 1e-4, up to 30 epochs) to avoid forgetting. Fine-tuning is skipped if there is no own data.

### From Keras to the microcontroller

[export.py](../ml/export.py) does the following:

1. **BatchNorm folding.** Each Conv + BN pair becomes one conv with bias: `scale = γ / √(var + ε)`, `w' = w · scale`, `b' = β − mean · scale`. Weights are reordered to C layouts: conv `[cout][k][cin]`, dense `[out][in]`.
2. **Verification.** `c_forward()` is a NumPy copy of the C loops. It runs on 200 windows and is compared with Keras. If the maximum difference is above 1e-3, export stops and no headers are written.
3. **Headers.** It writes `model_weights_f32.h` (11,508 floats ≈ 46 KB) and `norm_params.h` for both boards.
4. **int8 reference.** It also writes `models/model_int8.tflite` (full int8 quantisation calibrated on 500 training windows). The firmware no longer uses it; `evaluate.py` does.

### The C inference engine

[model.cpp](../firmware/src/model.cpp): normalise → conv+ReLU → maxpool → conv+ReLU → maxpool → conv+ReLU → mean over time → dense+ReLU → dense → softmax. Two 2,048-float ping-pong buffers (16 KB total) hold the intermediate activations.

Why not TFLite-Micro? The ESP32 build of TFLite-Micro has no optimised (ESP-NN) kernels for the classic ESP32 and took about 1.07 s per window. The hand-written float32 loops take a few ms on the ESP32's FPU. The ESP32 version also compiles this file with `-O3` (the project default is `-Os`) and copies the weights from flash into RAM at start-up to avoid flash-cache misses. The STM32 version reads the weights directly from flash.

---

## 8. The ML pipeline (Python)

Run every script from the project root as `python ml/<script>.py`. All paths come from [config.py](../ml/config.py); you can redirect them with environment variables `FALL_DATA_DIR`, `FALL_MODELS_DIR`, `FALL_FIRMWARE_SRC` and `FALL_FIRMWARE_STM32_SRC` (useful for testing on scratch copies).

```
SisFall zip ─► download_sisfall.py ─► data/SisFall_dataset/
logger firmware ─► record.py ─────────► data/own/<subject>/<label>_<time>.csv
                                              │
                        preprocess.py ◄───────┘
                              │
                              ▼
                 data/processed/windows.npz
                              │
                        train.py ─► models/pretrained.keras, final.keras, norm.json, splits.json
                              │
                        export.py ─► models/model_int8.tflite
                              │      firmware/src/model_weights_f32.h, norm_params.h
                              │      firmware_stm32/src/stm32_model_weights_f32.h, stm32_norm_params.h
                              ▼
                 evaluate.py (metrics)   parity_check.py (board vs Keras)
```

### config.py

The single source of truth on the Python side: paths, `FS=50`, `WIN=128`, `PRE=64`, `N_CH=6`, `TRAIN_OFFSETS`, `CLASSES`, SisFall scaling, the SisFall → class mapping, the SisFall → device axis mapping, and the Stage 1/2 thresholds.

SisFall activity codes map to classes as follows:

| SisFall codes | Class |
|---|---|
| F01–F15 (all falls) | fall |
| D07–D11 (sitting down on chairs, including collapsing into one) | sit |
| D12–D14 (lying down, turning while lying) | lie |
| All other D codes (walking, jogging, stairs, bending, car, stumbling, jumping) | other |

`SISFALL_TO_DEVICE` maps SisFall axes onto your MPU-6050 axes: entry *i* is `(source_axis, sign)` for device axis *i*, applied to both accelerometer and gyroscope. The default is identity.

### download_sisfall.py

`python ml/download_sisfall.py <zip path or URL>`. Extracts into `data/`, refuses archives with path-traversal entries, checks that trial files (`*_SA01_R01.txt` style) exist, and reports counts. SisFall: Sucerquia et al., *Sensors* 2017; 23 young adults (SA) and 15 older adults (SE).

### record.py

`python ml/record.py --port COM5 --subject alice`. With the logger firmware flashed: type a label, press Enter to start, perform the activity, press Enter to stop. A background thread keeps only valid 7-column numeric lines (ignoring `#` comments). Trials shorter than 64 samples are discarded. Files are saved as `data/own/<subject>/<label>_<YYYYmmdd_HHMMSS>.csv` with header `t_ms,ax,ay,az,gx,gy,gz`. The label is the part of the filename before the first underscore.

| Label | What to perform (≈ 20+ trials each, ideally 3+ people) |
|---|---|
| fall | Simulated falls onto a thick mattress: forward, backward, sideways, from sitting. Stand still ~2 s before, stay down ~3 s after. |
| sit | Soft and heavy sits on chairs, sofa, bed edge |
| lie | Lying down on a bed, from sitting and from standing |
| other | Walking, stairs, bending, picking things up, jumping; also long normal-activity sessions (these drive the false-alarm estimate) |

### preprocess.py

`python ml/preprocess.py` (or `--check-axes`, `--seed N`).

For SisFall files: parse the first 6 integer columns (ADXL345 accelerometer, ITG3200 gyroscope), scale to g (`32/8192` g per LSB) and °/s (`4000/65536` per LSB), apply a 4th-order Butterworth low-pass at 20 Hz with zero-phase `filtfilt`, downsample 200 → 50 Hz, then remap axes.

For own CSVs: read the six named columns; they are already in device units at 50 Hz.

For each trial: find the sample with the highest `|a|`, cut the 5 jittered windows around it, plus 3 random windows for long "other" trials.

Output `data/processed/windows.npz`:

| Array | Shape / type | Content |
|---|---|---|
| `X` | float32 (n, 128, 6) | Raw windows in device units |
| `y` | int64 (n,) | Class index |
| `subject` | str (n,) | `SA01`, `SE05`, `own_alice`, … |
| `source` | str (n,) | `sisfall` or `own` |
| `offset` | int64 (n,) | Jitter used; 0 = centred (only these are used for validation and testing) |

`--check-axes` prints mean gravity during the first second of SisFall walking trials (D01) after remapping. Compare it with what the logger shows while you stand still, and edit `SISFALL_TO_DEVICE` until they agree.

### common.py

- `split_subjects()`: **subject-wise** 70/15/15 split, so no person appears in both training and test. SisFall subjects and own subjects are split separately, so each split gets both. A group with fewer than 3 people goes entirely to training.
- `normalize()`: applies `(X − mean) / std` and reshapes to the model's `(n, 128, 1, 6)` input.
- `select()`: mask by subject list; `eval_only=True` keeps only offset-0 windows.
- `report()`: prints the confusion matrix, accuracy, fall sensitivity, fall specificity, and how often each non-fall class is misread as a fall.

### train.py

`python ml/train.py [--epochs 40] [--ft-epochs 30] [--seed 0]`. Splits subjects, computes normalisation from un-jittered training windows (std + 1e-6), pre-trains, optionally fine-tunes, and reports test metrics overall and per source.

Outputs in `models/`: `splits.json`, `norm.json`, `pretrained.keras`, `final.keras`.

### export.py

`python ml/export.py`. See [From Keras to the microcontroller](#from-keras-to-the-microcontroller). It requires exactly 3 Conv+BN blocks and 2 Dense layers, and exits with an error otherwise.

### evaluate.py

`python ml/evaluate.py`.

1. Float32 Keras model on test subjects (centred windows only): full report.
2. int8 TFLite model on the same data: full report, accuracy drop in percentage points, and how often float and int8 agree on the top class.
3. **Stream simulation:** runs a Python copy of the Stage 1 trigger (`find_triggers`) over every non-fall own recording from test subjects, classifies each trigger, and reports CNN fall alarms per hour. This number is *before* the stillness check, so the real device rate should be lower. It needs own recordings from at least 3 people, so that one lands in the test split.

Note that this script still evaluates the int8 model, while the firmware now runs float32. The float32 report is the one that matches the device.

### parity_check.py

`python ml/parity_check.py --port COM5 [--n 10]`. Close the serial monitor first. Sends test windows to the board in parity mode and compares the board's probabilities with Keras. It reports the maximum difference, top-class agreement and mean inference time. Expect very small differences, because both run float32.

---

## 9. ESP32 firmware

Build system: PlatformIO, `espressif32@^8.0.0` (Arduino core 3.x), board `esp32dev`, serial at 115200 baud. One `main.cpp` calls `appSetup()`/`appLoop()`; each environment links a different app file.

| Env | Command | Links | Purpose |
|---|---|---|---|
| `logger` | `pio run -e logger -t upload` | main, imu, app_logger | Streams `t_ms,ax,ay,az,gx,gy,gz` at 50 Hz; LED blinks at 1 Hz |
| `diag` | `pio run -e diag -t upload` | main, app_diag | Wiring finder (below) |
| `detector` | `pio run -e detector -t upload` | everything except logger/diag | The real device |

The detector env adds `NimBLE-Arduino@^2.1.0` (1.4.x crashes during Bluetooth controller init on core 3.x), runs `embed_web.py` before each build, and uses the `huge_app.csv` partition table, because BLE + Wi-Fi + web server don't fit the default app partition.

### File by file

- **[config.h](../firmware/src/config.h)**: pins, sample rate, every threshold from sections 5 and 6, `BLE_DEVICE_NAME = "FallGuard"`.
- **[imu.cpp](../firmware/src/imu.cpp)**: starts I²C on the configured pins at 400 kHz, tries address 0x68 then 0x69, sets ±16 g, ±2000 °/s and the 21 Hz filter. If the sensor isn't found it prints an I²C scan of every address that responds, and the app retries every second.
- **[app_diag.cpp](../firmware/src/app_diag.cpp)**: the breakout has pull-up resistors on SDA/SCL, so a GPIO wired to the powered module reads HIGH even with the internal pull-down on. It lists those pins, then tries an I²C scan on every pair of them and prints `FOUND device 0x68 with SDA=GPIOx SCL=GPIOy`. This is how the swapped SDA/SCL wiring was found.
- **[detector.cpp](../firmware/src/detector.cpp)**: section 5.
- **[model.cpp](../firmware/src/model.cpp)**: section 7. If `model_weights_f32.h` doesn't define `MODEL_F32_AVAILABLE`, stub functions make the device run rule-only.
- **[app_detector.cpp](../firmware/src/app_detector.cpp)**: section 6, plus:
  - The main loop samples on a fixed `micros()` schedule (`nextUs += 20000`), so timing doesn't drift.
  - Every CNN run is printed (`# impact -> fall=0.97 sit=0.01 …`) and sent as an impact event.
  - Every 5th sample (10 Hz) is packed as a motion packet, sent over BLE if a client is connected and pushed into the Wi-Fi ring buffer.
  - Battery: `analogReadMilliVolts × 2` if `PIN_BATT_ADC ≥ 0`, otherwise `-1`.
  - Serial log lines start with `#`, so `record.py` and other tools ignore them.
- **[ble_alert.cpp](../firmware/src/ble_alert.cpp)**: NimBLE GATT server, transmit power 9 dBm, advertises name and service UUID. Advertising restarts on connect, so a second phone or caregiver can connect too. Section 11 covers the characteristics.
- **[wifi_link.cpp](../firmware/src/wifi_link.cpp)**: compiled only if `wifi_secrets.h` exists (otherwise everything is a no-op and boot prints `# Wi-Fi disabled`). Joins the network in station mode with hostname `fallguard` and auto-reconnect. Once connected it starts the HTTP server on port 80 and registers mDNS `fallguard.local` with an `_http._tcp` service, re-registered on every reconnect. It keeps the latest status, alert and impact JSON plus a 64-entry ring buffer (6.4 s) of motion samples.
- **[embed_web.py](../firmware/embed_web.py)**: gzips `../web/index.html` (level 9, fixed mtime) into the C array `kWebIndexGz`. It only rewrites the header when the content changed, which keeps incremental builds fast. It can also be run directly with `python firmware/embed_web.py`.

### Wi-Fi setup

```bash
cp firmware/src/wifi_secrets.example.h firmware/src/wifi_secrets.h   # then edit SSID and password
```

Use a 2.4 GHz network; the ESP32 has no 5 GHz radio. Non-ASCII SSIDs can be written as UTF-8 escapes (for example `"\xCF\x80"` for π). `wifi_secrets.h` is git-ignored, so never commit it.

---

## 10. STM32 firmware

PlatformIO `ststm32@^20.0.0` (STM32duino core 4.x), board `nucleo_f401re`, upload via ST-LINK. The linker flags `-u _printf_float -u _scanf_float` are needed because newlib-nano otherwise can't print or parse floats.

| Env | Purpose |
|---|---|
| `stm32_logger` | Same CSV stream as the ESP32 logger; `record.py` works unchanged |
| `stm32_detector` | Detection + HM-10 BLE alerts |

`stm32_fall_detector.cpp`, `stm32_model_cnn_f32.cpp` and the thresholds in `stm32_config.h` are the same as the ESP32 versions (the model reads weights from flash instead of copying them to RAM). The differences:

- **BLE through an HM-10 module** ([stm32_ble_hm10.cpp](../firmware_stm32/src/stm32_ble_hm10.cpp)). It exposes the same functions as `ble_alert.h`, so the app logic is the same. It owns USART1 (PA10/PA9) at 9600 baud. At boot it sends `AT`, `AT+NAME<name>`, `AT+NOTI1` (report `OK+CONN`/`OK+LOST` on connect and disconnect) and `AT+RESET`. Genuine HM-10s want no line ending; clones (BT05, MLT-BT05, AT-09, JDY-08) need `\r\n`, so set `HM10_AT_CRLF = true` if the module doesn't rename itself to `FallGuard`. If the module never reports connection events, the firmware assumes a phone may be listening and keeps re-sending alerts. Any received line or text ending in `ACK`/`ack` (except module `OK…` replies) counts as an acknowledgement.
- **No Wi-Fi, no motion stream, no impact events** over BLE. The serial log still shows impacts.
- Status is sent only every 60 s, not on every state change.
- Battery reading: `analogRead × 3300 / 1023 × 2` (10-bit ADC).
- Button is the on-board B1 (PC13, active LOW).

---

## 11. Communication protocols

### ESP32 BLE GATT service

Device name `FallGuard`. Service UUID `6e3f0001-8b1a-4c2d-9a7e-1f2b3c4d5e6f`. The other characteristics differ only in the first group.

| Characteristic | UUID prefix | Properties | Content |
|---|---|---|---|
| alert | `6e3f0002` | read, notify | Alert JSON (below). Starts as `{}`. |
| status | `6e3f0003` | read, notify | Status JSON or impact-event JSON |
| ack | `6e3f0004` | write, write-without-response | Any non-empty value acknowledges the active alert |
| motion | `6e3f0005` | notify | 14-byte binary motion packet at 10 Hz |

### JSON messages (all transports)

**Alert** (fall):
```json
{"id":3,"type":"fall","conf":0.91,"angle":72,"still":0.85,"model":true,"uptime_s":1200}
```
- `id`: increments with every alert since boot; receivers use it to ignore 5-second re-sends.
- `type`: `fall`, `sos`, `cancel` (wearer pressed the button) or `acked`.
- `conf`: CNN fall probability. `angle`: trunk tilt in degrees. `still`: still fraction. `model`: false if running rule-only.
- An SOS carries only `id`, `type`, `uptime_s`. `cancel`/`acked` carry only `id` and `type`.

**Status:**
```json
{"state":"idle","uptime_s":60,"batt_mv":-1}
```
`state` is `idle`, `pre_alert` or `alerted`. `batt_mv` is −1 when not measured.

**Impact event** (ESP32 only, after every CNN run, sent on the status characteristic):
```json
{"event":"impact","probs":[0.970,0.010,0.010,0.010],"fall_like":true,"model":true,"uptime_s":512}
```

### Motion packet (14 bytes, little-endian)

| Bytes | Type | Content |
|---|---|---|
| 0–5 | 3 × int16 | ax, ay, az in mg (milli-g) |
| 6–11 | 3 × int16 | gx, gy, gz in 0.1 °/s |
| 12 | uint8 | Detector state (0 Idle, 1 WaitPost, 2 Stillness, 3 Cooldown) |
| 13 | uint8 | Sequence number (wraps at 256) |

Values are clamped to the int16 range.

### HM-10 (STM32)

Service `FFE0`, characteristic `FFE1` (notify + write-without-response). Alerts and status messages are the same JSON, sent as newline-terminated lines. The module splits them into 20-byte notifications, and receivers re-join them on `\n`. Write `ACK` to acknowledge.

### Wi-Fi HTTP API (ESP32)

Reachable at `http://fallguard.local/` or `http://<ip>/` (the IP is printed over serial). Some Android phones don't resolve `.local` names; use the IP there. JSON endpoints send `Access-Control-Allow-Origin: *`.

| Method and path | Response |
|---|---|
| `GET /` or `/index.html` | The gzipped dashboard |
| `GET /live?since=SEQ` | `{"seq","uptime_s","m":[[ax,ay,az,gx,gy,gz,stage],…],"status":{…},"alert":{…},"impact":{…},"impact_n":N,"wifi":{"ip","rssi"}}`. `m` contains motion samples newer than `SEQ` (mg and 0.1 °/s). If `SEQ` is unknown or older than the buffer, the last 20 samples (2 s) are sent. |
| `GET /status` | `{"status":{…},"alert":{…},"wifi":{"ip","rssi"}}` |
| `POST /ack` | `{"ok":true}`; acknowledges the active alert, same as the BLE ack |
| anything else | 404 |

`impact_n` counts impact events, so a poller can tell a new impact from a repeat of the last one.

---

## 12. Receivers: laptop script and web dashboard

### receiver/ble_receiver.py

```bash
python receiver/ble_receiver.py           # ESP32
python receiver/ble_receiver.py --hm10    # STM32 + HM-10
python receiver/ble_receiver.py --no-ack  # don't auto-acknowledge
```

It scans for a device named `FallGuard` (or advertising the service), connects, subscribes to notifications, and on the ESP32 also reads the current alert value in case an alert was already active. Each new alert `id` prints a banner and rings the terminal bell. By default it acknowledges automatically, which stops the buzzer. Re-sends with the same `id` are ignored. If the connection drops, it rescans and reconnects.

### web/index.html (the dashboard)

A single self-contained HTML file with no external dependencies. It has light and dark themes and these panels:

- **Header:** connection pill, Demo mode button, Connect device button.
- **Banner:** the 15 s countdown during pre-alert, or the alert with an Acknowledge button.
- **Device card:** alert state, uptime, battery, last update, and the four pipeline stages with the current one highlighted.
- **Last impact card:** probability bars for fall/sit/lie/other and the verdict.
- **Live motion chart:** `|a|` over the last 10 s on a canvas, with dashed lines at 0.6, 2.5 and 3 g. Below it, readouts of ax/ay/az, `|a|`, `|gyro|` and tilt.
- **Event log:** the last 200 events, newest first.

New alerts also play three beeps and show a browser notification if permission was granted.

It picks a transport automatically:

1. **Served by the ESP32** (`http://fallguard.local/`): it detects this by fetching `/live`, then polls `/live` about every 100 ms and acknowledges with `POST /ack`. It works on any phone or laptop browser. On the first load it only shows an alert that is still active. On errors it backs off up to 5 s and shows "device not responding".
2. **Web Bluetooth** (Chrome or Edge on desktop or Android): serve the file locally with `cd web && python -m http.server 8000`, open `http://localhost:8000`, and click **Connect device**. Web Bluetooth requires localhost or HTTPS, not `file://`. If the connection drops, it reconnects with backoff (up to 10 tries).
3. **Demo mode** (button or `#demo` in the URL): simulated data that alternates walking, a hard sit (the CNN says "sit", so it is ignored), and a fall (free-fall dip, impact, lying still, countdown, alert). Acknowledge works in demo mode too.

Because the page is compiled into the firmware, **rebuild and reflash the detector after editing `web/index.html`**.

---

## 13. Current results

From the latest training run (`models_train.log`), trained on **SisFall only**: no own recordings exist yet in `data/own/`, so fine-tuning was skipped. 38 subjects were split 26 / 6 / 6. Pre-training used 17,242 windows.

Test set: 6 held-out SisFall subjects, 1,626 centred windows, float32 model.

| true ↓ / predicted → | fall | sit | lie | other |
|---|---|---|---|---|
| **fall** | 447 | 0 | 1 | 2 |
| **sit** | 0 | 141 | 0 | 9 |
| **lie** | 0 | 1 | 85 | 4 |
| **other** | 15 | 28 | 0 | 893 |

| Metric | Value | Target |
|---|---|---|
| Accuracy (4 classes) | 96.3% | |
| Fall sensitivity | 99.3% | ≥ 95% |
| Fall specificity | 98.7% | ≥ 95% |
| sit misread as fall | 0.0% | |
| lie misread as fall | 0.0% | |
| other misread as fall | 1.6% | |

These are window-level numbers on SisFall, whose sensor was a different device. Results on your own MPU-6050 will differ until you record your own data and fine-tune. The 1.6% "other" false positives are further reduced on the device by the Stage 1 trigger (most windows never reach the CNN) and the Stage 3 stillness check.

---

## 14. End-to-end workflow

```bash
# 0. Install
pip install -r ml/requirements.txt
pip install platformio

# 1. Check wiring (ESP32): find SDA/SCL if unsure, then check axes with the logger
cd firmware && pio run -e diag -t upload && pio device monitor
cd firmware && pio run -e logger -t upload && pio device monitor
#    Stand still wearing the device: one axis should read about ±1 g.
#    STM32: cd firmware_stm32 && pio run -e stm32_logger -t upload && pio device monitor

# 2. SisFall pre-training data
python ml/download_sisfall.py path/to/SisFall_dataset.zip
python ml/preprocess.py --check-axes       # edit SISFALL_TO_DEVICE until it matches step 1

# 3. Record your own data (logger still flashed, serial monitor closed)
python ml/record.py --port COM5 --subject alice

# 4. Train, export, evaluate
python ml/preprocess.py
python ml/train.py
python ml/export.py
python ml/evaluate.py

# 5. Wi-Fi (ESP32, optional): create firmware/src/wifi_secrets.h from the example

# 6. Flash the detector
cd firmware && pio run -e detector -t upload && pio device monitor
#    STM32: cd firmware_stm32 && pio run -e stm32_detector -t upload && pio device monitor

# 7. Check the board matches Python (close the monitor first)
python ml/parity_check.py --port COM5

# 8. Receive alerts
python receiver/ble_receiver.py            # or open http://fallguard.local/
```

### Live acceptance test

Run 10 mattress falls, 10 hard sits, 10 lie-downs and 30 minutes of normal activity. Count detected falls, false alarms, and the delay from impact to the alert arriving (expected ≈ 18.5 s). Also check that a short press during the 15 s pre-alert cancels it.

---

## 15. Changing things safely

### Thresholds

Detector thresholds live in **three** places that must match:

| Constant | [ml/config.py](../ml/config.py) | [firmware/src/config.h](../firmware/src/config.h) | [firmware_stm32/src/stm32_config.h](../firmware_stm32/src/stm32_config.h) |
|---|---|---|---|
| Free-fall, impact, impact-only, free-fall window | ✓ | ✓ | ✓ |
| `FALL_PROB_MIN` | ✓ | ✓ | ✓ |
| Stage 3 (still tolerance, gyro limit, fractions, angle, cooldown) | | ✓ | ✓ |
| Alert timings | | ✓ | ✓ |

The Python values only affect `evaluate.py`'s simulation. The dashboard also hard-codes 15 s (`PRE_ALERT_S`) and the 0.6 / 2.5 / 3 g chart lines.

### Window, channels, classes

`WIN`, `PRE`, `N_CH`, `CLASSES` and `FALL_IDX` are written into `norm_params.h` by `export.py`, so the firmware follows automatically after re-running preprocess → train → export. Watch for these hard-coded spots:

- The detector's ring buffer is 256 samples; `WIN` must stay comfortably below that.
- The pre-fall orientation uses the first 25 samples of the window.
- Parity mode and the impact JSON assume 6 channels.
- The dashboard and receiver hard-code the four class names.

### Model architecture

If you change `build_model()`, update **all three** implementations of the forward pass: `c_forward()` in `export.py`, [model.cpp](../firmware/src/model.cpp), and [stm32_model_cnn_f32.cpp](../firmware_stm32/src/stm32_model_cnn_f32.cpp). `export.py` refuses to write headers if its NumPy copy disagrees with Keras, and `fold_layers()` refuses anything other than 3 Conv+BN blocks and 2 Dense layers. After flashing, run `parity_check.py` to confirm the C code matches as well.

### Mounting orientation

If you mount the device differently, re-record your own data in the new orientation, re-check `SISFALL_TO_DEVICE`, and retrain. The dashboard's tilt readout assumes −Y points down.

### Dashboard

Edit `web/index.html`, test it with `#demo` locally, then rebuild and reflash the ESP32 detector.

---

## 16. Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| `# MPU-6050 not found` repeating | Wiring or power. Read the printed I²C scan. On the ESP32, flash `env:diag` to find which GPIOs the sensor is on, and set `PIN_SDA`/`PIN_SCL`. |
| `# no model: run ml/export.py` | `model_weights_f32.h` has no weights. Run train + export. The device still works rule-only. |
| Logger shows gravity on an unexpected axis | Mounting differs from training. See [Mounting orientation](#mounting-orientation). |
| `record.py` warns "no data from the logger" | Wrong port, the detector firmware is flashed instead of the logger, or a serial monitor is holding the port. |
| `parity_check.py`: "no reply" | Serial monitor still open, or the detector firmware isn't flashed. |
| `export.py`: "float engine mismatch" | The architecture changed without updating `c_forward()`. |
| `# Wi-Fi disabled` at boot | `wifi_secrets.h` is missing. Copy it from the example. |
| `# Wi-Fi: still connecting (status N)` | Wrong SSID or password, or a 5 GHz-only network. |
| `fallguard.local` doesn't open | The device doesn't resolve `.local` names (common on Android). Use the IP printed over serial. |
| Dashboard over Bluetooth: "no Web Bluetooth" | Use Chrome or Edge, served from localhost or HTTPS, not `file://`. |
| Dashboard shows "no live motion stream" | Old firmware. Reflash the detector. |
| HM-10 doesn't rename to FallGuard | Clone module: set `HM10_AT_CRLF = true`. Check it is at 9600 baud. |
| Build fails in `esp_bt_controller_init` / crash at boot | NimBLE 1.4.x on Arduino core 3.x. Keep `NimBLE-Arduino@^2.1.0`. |
| Upload fails on ESP32 | Hold BOOT while uploading on some DevKits; check the USB-UART driver. |
| Too many false alarms | Record more `other`/`sit` data and fine-tune; raise `FALL_PROB_MIN` or `IMPACT_ONLY_G` (in all three config files). |

---

## 17. Limitations and known gaps

- **No own data yet.** The current model is trained on SisFall only, recorded with a different sensor. Recording and fine-tuning is the most important next step.
- **Not yet built:** SIM800L GSM/SMS fallback, battery power with deep sleep (the MPU-6050 INT pin is wired for motion wake-up), a phone app.
- **Alerts only reach someone if a receiver is connected.** On the ESP32, BLE alerts are re-sent every 5 s while a client is connected, and the latest alert stays readable over Wi-Fi. There is no store-and-forward to the cloud.
- **The Wi-Fi API has no authentication.** Anyone on the same network can view the data or acknowledge an alert with `POST /ack`. Use a trusted network.
- **The STM32 build has fewer features**: no Wi-Fi, no live motion, no impact events over BLE, status only every 60 s.
- **`evaluate.py` measures the int8 model**, not the float32 engine the firmware runs. The float32 report it prints first is the relevant one.

---

## 18. Glossary

| Term | Meaning |
|---|---|
| ADL | Activities of daily living: everything that is not a fall |
| BLE / GATT | Bluetooth Low Energy / its service–characteristic data model |
| BN folding | Merging a BatchNorm layer into the previous convolution's weights and bias |
| CNN | Convolutional neural network |
| DLPF | Digital low-pass filter inside the MPU-6050 |
| g | Acceleration of gravity (9.80665 m/s²); standing still reads about 1 g |
| HM-10 | Cheap BLE module that acts as a wireless serial port |
| IMU | Inertial measurement unit (here the MPU-6050) |
| mDNS | Lets `fallguard.local` resolve on a local network without a DNS server |
| Parity check | Feeding the same windows to the board and to Python to confirm identical outputs |
| Sensitivity | Share of real falls that are detected |
| SisFall | Public fall and ADL dataset (Sucerquia et al., 2017) used for pre-training |
| Specificity | Share of non-falls that are correctly not called falls |
| Subject-wise split | Splitting by person, so the test set contains only people the model never saw |
| TFLite-Micro | TensorFlow Lite for microcontrollers; no longer used by the firmware |
