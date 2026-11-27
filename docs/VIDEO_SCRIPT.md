# FallGuard: 3-minute video script (ESP32)

The spoken part is about 440 words, which takes roughly 3 minutes at a normal pace. Each section gives what to show and what to say.

---

### 0:00–0:15 · Opening

**Show:** a close-up of the device clipped to a belt, then a quick cut to the dashboard flashing red with "FALL DETECTED".

**Say:**
"Every year, millions of older people fall, and many can't get up or call for help. What if a small device on your belt could notice the fall and call for help for you? This is FallGuard."

---

### 0:15–0:40 · Introduction

**Show:** the hardware on a desk. Point to each part as you name it.

**Say:**
"FallGuard is a wearable fall detector built on an ESP32 microcontroller. An MPU-6050 motion sensor measures acceleration and rotation fifty times a second. A small neural network runs on the ESP32 itself and decides whether a movement was a real fall, or just sitting down or lying down. There's also a button, a buzzer and an LED. Alerts go out over Bluetooth and Wi-Fi, so it works without the cloud or an internet connection."

---

### 0:40–1:25 · Demo

**Show:** a phone opening `http://fallguard.local/`. Then walk, sit down hard on a chair, and fall onto a mattress. Use a split screen of you and the dashboard.

**Say:**
"The device hosts its own dashboard. I just open fallguard.local on any phone on the same Wi-Fi.

First, I'll sit down hard. The impact is strong enough to trigger the detector, but the neural network classifies it as 'sit', so it's ignored. No false alarm.

Now a real fall. The detector spots the impact, the network says 'fall', and the device checks for two seconds whether I'm lying still or have tipped over. Then a fifteen-second countdown starts and the device beeps. If I'm okay, one press of the button cancels it.

This time I'll let it run out. The alert goes to the dashboard and to a laptop over Bluetooth, and it keeps repeating until a caregiver taps Acknowledge. Holding the button for three seconds sends a manual SOS at any time."

> If you can't film a live fall safely, use the dashboard's **Demo mode** for this part.

---

### 1:25–2:00 · What the dashboard shows

**Show:** a slow pan across the dashboard, highlighting each panel as you mention it.

**Say:**
"The live graph shows the total acceleration over the last ten seconds. Standing still reads about one g. The orange dashed line at 0.6 g marks free-fall, and the red lines at 2.5 and 3 g mark an impact. A fall looks like a dip followed by a sharp spike.

Below the graph are the raw readings for each axis, the rotation speed, and how far my body is tilted.

The Device card shows the alert state, the uptime, and which detection stage is active right now: monitoring, CNN check, post-impact check or cooldown. The CNN card shows the model's confidence for fall, sit, lie and other after every impact. The event log keeps a timestamped history."

---

### 2:00–2:30 · Tools and technology

**Show:** quick cuts of VS Code with PlatformIO, the Python training output, and a diagram: sensor → trigger → CNN → check → alert.

**Say:**
"The firmware is written in C++ with the Arduino framework and built with PlatformIO. Bluetooth runs on the NimBLE library, and the ESP32 serves the web dashboard itself.

The model is trained in Python with TensorFlow and Keras. It's a 1D convolutional neural network with only about twelve thousand parameters. At first we used TensorFlow Lite Micro, but it took about a second per prediction. So we wrote our own small inference engine in C, which takes just a few milliseconds. Every export is automatically checked against the original model."

---

### 2:30–2:52 · The dataset

**Show:** sample SisFall acceleration plots, a fall next to a sit, and the results table.

**Say:**
"To train the network we used SisFall, a public research dataset. It has thirty-eight participants, both young and older adults, wearing a sensor at the waist. They performed fifteen types of simulated falls and nineteen everyday activities, recorded at 200 samples per second.

We filtered the data, downsampled it to 50 hertz to match our sensor, and cut 2.56-second windows centred on each impact. We split by person, so the model is always tested on people it has never seen. On those unseen people, it caught 99.3 percent of falls."

---

### 2:52–3:00 · Closing

**Show:** the device on the belt and the project title card with the GitHub link.

**Say:**
"FallGuard: fall detection that runs on the device itself and calls for help. Thanks for watching."

---

**Tips for filming:**
- Clip the device in the same orientation you use for training.
- Fall onto a thick mattress.
- Record the demo in one take with the phone screen visible.
