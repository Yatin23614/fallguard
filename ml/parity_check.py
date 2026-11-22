"""Check the ESP32 gives the same answer as the Python model.

Flash the detector firmware, then:
    python ml/parity_check.py --port COM5 [--n 10]

It sends test windows over serial as raw (unnormalised) samples:
    X,ax,ay,az,gx,gy,gz       (128 lines)
The firmware normalises and runs the float32 CNN exactly as it does live,
and replies with
    P,p_fall,p_sit,p_lie,p_other,us
Reference: the Keras model (the firmware engine is a float32 copy of it).
"""
import argparse
import os
import time

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")

import numpy as np
import serial
import tensorflow as tf

import config as C
from common import load_json, load_windows, normalize, select


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--n", type=int, default=10)
    args = ap.parse_args()

    norm = load_json(C.MODELS_DIR / "norm.json")
    splits = load_json(C.MODELS_DIR / "splits.json")
    d = load_windows()
    m = select(d, splits["test"] or splits["train"], eval_only=True)
    idx = np.random.default_rng(1).choice(np.flatnonzero(m), min(args.n, m.sum()),
                                          replace=False)
    fmodel = tf.keras.models.load_model(C.MODELS_DIR / "final.keras")

    ser = serial.Serial(args.port, args.baud, timeout=5)
    time.sleep(2)
    ser.reset_input_buffer()
    diffs, agree, times = [], 0, []
    for i in idx:
        w = d["X"][i]
        ref = fmodel.predict(normalize(w[None], norm), verbose=0)[0]
        for row in w:
            ser.write(("X," + ",".join(f"{v:.5f}" for v in row) + "\n").encode())
        ser.flush()
        reply = None
        t_end = time.time() + 5
        while time.time() < t_end:
            line = ser.readline().decode(errors="ignore").strip()
            if line.startswith("P,"):
                reply = [float(v) for v in line.split(",")[1:]]
                break
        if reply is None:
            print(f"window {i}: no reply")
            continue
        dev, us = np.asarray(reply[:len(C.CLASSES)]), reply[len(C.CLASSES)]
        diffs.append(np.abs(dev - ref).max())
        agree += int(dev.argmax() == ref.argmax())
        times.append(us)
        print(f"window {i} true={C.CLASSES[d['y'][i]]:>5}  "
              f"esp32={np.round(dev, 3)}  python={np.round(ref, 3)}  {us / 1000:.1f} ms")
    if diffs:
        print(f"\nmax |diff|={max(diffs):.4f}  argmax agreement {agree}/{len(diffs)}  "
              f"mean inference {np.mean(times) / 1000:.1f} ms")


if __name__ == "__main__":
    main()
