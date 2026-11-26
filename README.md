# FallGuard: fall-detection wearable (ESP32 or STM32 + MPU-6050 + 1D-CNN + BLE)

A waist-worn device that detects falls and sends a BLE alert to a phone or laptop. A small 1D-CNN running on the microcontroller separates real falls from deliberately **sitting** or **lying down**.

There are two firmware builds. Both share the same Python training pipeline and the same detection logic.

| | ESP32 | STM32 Nucleo-F401RE |
|---|---|---|
| Folder | [firmware/](firmware/) | [firmware_stm32/](firmware_stm32/) (all files prefixed `stm32_`) |
| BLE | Built-in radio, custom GATT service | External **HM-10** BLE-UART module |
| CNN runtime | TFLite-Micro, int8 (`model_data.h`) | Library-free float32 engine (`stm32_model_weights_f32.h`) |
| Upload / serial | USB (CP2102/CH340) | On-board ST-LINK (USB virtual COM port) |
| Receiver | `python receiver/ble_receiver.py` | `python receiver/ble_receiver.py --hm10` |

```
MPU-6050 @50 Hz ─► Stage 1: impact trigger (free-fall < 0.6 g, then > 2.5 g; or > 3 g)
                ─► Stage 2: 1D-CNN on a 2.56 s window centred on the impact
                            → fall / sit / lie / other
                ─► Stage 3: 2 s post-impact check (trunk tilt ≥ 45° or lying still)
                ─► 15 s pre-alert (beeping, button cancels) ─► BLE alert, re-sent until acked
Hold button 3 s = manual SOS.
```

## Pin connections

### ESP32 (DevKit v1 / esp32dev)

| Part | Part pin | ESP32 pin |
|---|---|---|
| MPU-6050 (GY-521) | VCC | 3V3 |
| | GND | GND |
| | SDA | GPIO22 (as currently wired; set in `config.h`) |
| | SCL | GPIO21 |
| | INT | GPIO19 (reserved for motion-wake) |
| | AD0 | GND or unconnected (I²C address 0x68) |
| Push button | One leg | GPIO4 |
| | Other leg | GND (uses the internal pull-up) |
| Active buzzer | + | GPIO25 |
| | − | GND |
| LED | | GPIO2 (on-board) |
| Battery sense (optional) | 1:2 divider midpoint | GPIO35. Set `PIN_BATT_ADC = 35`. |

The BLE radio is built in, so no extra wiring is needed. Pins are set in [firmware/src/config.h](firmware/src/config.h).

### STM32 Nucleo-F401RE

| Part | Part pin | Nucleo pin (Arduino header / MCU pin) |
|---|---|---|
| MPU-6050 (GY-521) | VCC | 3V3 |
| | GND | GND |
| | SDA | D14 / PB9 (I2C1 SDA) |
| | SCL | D15 / PB8 (I2C1 SCL) |
| | INT | D7 / PA8 (reserved for motion-wake) |
| | AD0 | GND or unconnected (I²C address 0x68) |
| HM-10 BLE module | VCC | 5V (most HM-10 breakouts have a regulator; use 3V3 for a bare module) |
| | GND | GND |
| | TXD | D2 / PA10 (USART1 RX) |
| | RXD | D8 / PA9 (USART1 TX) |
| Push button | | On-board blue USER button B1 (PC13). No wiring needed. |
| Active buzzer | + | D5 / PB4 |
| | − | GND |
| LED | | On-board green LD2 (D13 / PA5) |
| Battery sense (optional) | 1:2 divider midpoint | A0 / PA0. Set `PIN_BATT_ADC = PA0`. |

Serial logging and the parity check use the ST-LINK USB virtual COM port (USART2, PA2/PA3). Don't wire anything to D0 or D1. Pins are set in [firmware_stm32/src/stm32_config.h](firmware_stm32/src/stm32_config.h).

> **HM-10 clones:** a genuine HM-10 (CC2541) takes AT commands with no line ending. Clones such as BT05, MLT-BT05, AT-09 and JDY-08 need `\r\n`. If the module doesn't rename itself to `FallGuard` after the first boot, set `HM10_AT_CRLF = true` in `stm32_config.h`. The module's UART must be at 9600 baud, the factory default.

**For both boards:** clip the board to the belt **firmly** and **always in the same orientation**, because the model depends on it. Use the same orientation for recording data and for live use.

## Setup

```bash
pip install -r ml/requirements.txt      # tensorflow, numpy, scipy, pyserial, bleak
pip install platformio
```

## Workflow

### 1. Flash the logger and check the axes
```bash
cd firmware          && pio run -e logger -t upload       && pio device monitor   # ESP32
cd firmware_stm32    && pio run -e stm32_logger -t upload && pio device monitor   # STM32
```
Stand still while wearing the device. One axis should read about ±1 g. Both boards stream the same CSV format, so `record.py` works with either one. On the Nucleo, the port is the ST-LINK virtual COM port.

### 2. Get SisFall (pre-training data)
Download the SisFall zip (Sucerquia et al., 2017) from the authors' page or a mirror, then:
```bash
python ml/download_sisfall.py path/to/SisFall_dataset.zip
python ml/preprocess.py --check-axes
```
`--check-axes` prints gravity in the SisFall data after it has been mapped to the device frame. Edit `SISFALL_TO_DEVICE` in [ml/config.py](ml/config.py) until it matches what your logger shows while you stand upright.

### 3. Record your own data (fine-tuning)
With the logger flashed:
```bash
python ml/record.py --port COM5 --subject alice
```
Record about 20+ trials of each label per person, ideally with 3 or more people:

| label | what to do |
|---|---|
| `fall` | Simulated falls onto a thick mattress: forward, backward, sideways, from sitting. Stay down for about 3 s. |
| `sit` | Sit down softly and heavily on chairs, a sofa and a bed edge. |
| `lie` | Lie down on a bed, both from sitting and from standing. |
| `other` | Walking, stairs, bending, picking things up, jumping. Also record long normal-activity sessions. |

Files go to `data/own/<subject>/<label>_<timestamp>.csv`.

### 4. Train, export, evaluate
```bash
python ml/preprocess.py     # → data/processed/windows.npz
python ml/train.py          # SisFall pre-train → own-data fine-tune (subject-wise split)
python ml/export.py         # ESP32: int8 TFLite → firmware/src/model_data.h + norm_params.h
                            # STM32: float32 weights → firmware_stm32/src/stm32_model_weights_f32.h
                            #        + stm32_norm_params.h (checked against Keras first)
python ml/evaluate.py       # confusion matrix, fall sensitivity/specificity, float vs int8,
                            # simulated false alarms/hour on own non-fall recordings
```
Targets: fall sensitivity ≥ 95%, and specificity against sit/lie ≥ 95%.

### 5. Flash the detector
```bash
cd firmware       && pio run -e detector -t upload       && pio device monitor   # ESP32
cd firmware_stm32 && pio run -e stm32_detector -t upload && pio device monitor   # STM32
```
The serial monitor logs every impact with its class probabilities, and every confirmed fall. If no model has been exported, the detector still runs on the trigger and post-impact rules alone, and logs a warning.

On-device parity check (board vs Python, plus inference time). Close the serial monitor first:
```bash
python ml/parity_check.py --port COM5
```
The script compares against the Python int8 model. On the STM32, which runs in float32, expect small differences of around 0.01–0.02, but the predicted class should match.

### 6. Receive alerts
- **Laptop:** `python receiver/ble_receiver.py`. It prints alerts and status messages, auto-acks alerts (which stops the buzzer), and reconnects on its own.
- **Phone:** use nRF Connect. Connect to `FallGuard`, enable notifications on the alert characteristic, and write any value to the ack characteristic.
- **Wi-Fi:** the ESP32 joins the network set in `firmware/src/wifi_secrets.h`. That file is git-ignored; create it from [wifi_secrets.example.h](firmware/src/wifi_secrets.example.h), and use a 2.4 GHz network because the ESP32 has no 5 GHz radio. Once connected, `http://fallguard.local/status` (or `http://<device-ip>/status`, which is printed over serial) returns `{"status":{…},"alert":{…},"wifi":{"ip":…,"rssi":…}}`. Bluetooth alerting keeps working alongside it.
- **Web dashboard:** [web/index.html](web/index.html) shows live motion, each impact's CNN decision, the 15 s countdown, alerts with an Acknowledge button, and an event log. It talks to the ESP32 directly over Web Bluetooth, so it needs Chrome or Edge on desktop or Android.
  ```bash
  cd web && python -m http.server 8000
  ```
  Then open `http://localhost:8000` and click **Connect device**. Web Bluetooth only works on localhost or HTTPS, not from a `file://` path. Open `http://localhost:8000/#demo`, or click **Demo mode**, to show the whole flow with simulated data and no device.

| Characteristic | UUID | Properties | Content |
|---|---|---|---|
| service | `6e3f0001-8b1a-4c2d-9a7e-1f2b3c4d5e6f` | | |
| alert | `6e3f0002-…` | read, notify | `{"id":3,"type":"fall","conf":0.91,"angle":72,"still":0.85,"model":true,"uptime_s":1200}`. `type` is one of `fall`, `sos`, `cancel` or `acked`. |
| status | `6e3f0003-…` | read, notify | `{"state":"idle","uptime_s":60,"batt_mv":-1}`, sent every 60 s and whenever the alert state changes. After every CNN run it also sends `{"event":"impact","probs":[fall,sit,lie,other],"fall_like":true,…}`. |
| ack | `6e3f0004-…` | write | Any value acknowledges the alert. |
| motion | `6e3f0005-…` | notify | 10 Hz, 14 bytes little-endian: int16 ax, ay, az (mg); int16 gx, gy, gz (0.1 °/s); uint8 detector stage; uint8 sequence number. |

**STM32 + HM-10:**
- **Laptop:** run `python receiver/ble_receiver.py --hm10`.
- **Phone:** use nRF Connect or any "BLE serial terminal" app. Connect to `FallGuard`, enable notifications on characteristic `FFE1` (service `FFE0`), and send `ACK` to acknowledge.

Alerts and status messages use the same JSON as above, sent as newline-terminated lines on that single characteristic. The HM-10 splits them into 20-byte chunks, which the receiver joins back together.

## Live acceptance test
Run 10 mattress falls, 10 hard sits, 10 lie-downs and 30 minutes of normal activity. Count:
- detected falls,
- false alarms,
- the delay from impact to the alert reaching the receiver (about 1.3 s window + 2 s check + 15 s cancel period),
- whether a short button press during the 15 s pre-alert cancels it.

## Notes
- The model is a 1D CNN over time. It is written with `(k,1)` Conv2D kernels, so it converts to plain CONV_2D, MAX_POOL_2D, AVERAGE_POOL_2D, FULLY_CONNECTED and SOFTMAX ops that every TFLite-Micro build supports. It is about 20 KB as int8.
- Thresholds in `firmware/src/config.h`, `firmware_stm32/src/stm32_config.h` and `ml/config.py` mirror each other. Keep all three in sync.
- The STM32 runs the same CNN with a small library-free float32 engine, [stm32_model_cnn_f32.cpp](firmware_stm32/src/stm32_model_cnn_f32.cpp). `export.py` runs a NumPy copy of its exact loops and refuses to write the STM32 headers unless it matches Keras. If you change the model architecture, update both.
- Not yet included: a SIM800L GSM/SMS fallback, battery power with deep sleep (MPU-6050 motion-interrupt wake on GPIO19), and a phone app.
- This is a prototype, not a certified medical device.
