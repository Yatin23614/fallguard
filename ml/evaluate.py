"""Evaluate float vs int8 models on held-out subjects, and simulate the
on-device trigger over continuous own recordings to estimate false alarms/hour.

Usage:
    python ml/evaluate.py
"""
import os

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")

import numpy as np
import tensorflow as tf

import config as C
from common import load_json, load_windows, normalize, report, select
from preprocess import load_own_file, window_at


class Int8Model:
    def __init__(self, path):
        self.it = tf.lite.Interpreter(model_path=str(path))
        self.it.allocate_tensors()
        self.inp = self.it.get_input_details()[0]
        self.out = self.it.get_output_details()[0]

    def predict(self, Xn):
        s_in, z_in = self.inp["quantization"]
        s_out, z_out = self.out["quantization"]
        probs = []
        for x in Xn:
            q = np.clip(np.round(x / s_in + z_in), -128, 127).astype(np.int8)
            self.it.set_tensor(self.inp["index"], q[None])
            self.it.invoke()
            o = self.it.get_tensor(self.out["index"])[0].astype(np.float32)
            probs.append((o - z_out) * s_out)
        return np.asarray(probs)


def find_triggers(sig):
    """Python mirror of the firmware Stage-1 trigger (firmware/src/detector.cpp)."""
    mag = np.linalg.norm(sig[:, :3], axis=1)
    last_ff, events, i = -10**9, [], 0
    while i < len(mag):
        if mag[i] < C.FREEFALL_G:
            last_ff = i
        hit = (mag[i] > C.IMPACT_G and i - last_ff <= C.FREEFALL_WINDOW) or \
            mag[i] > C.IMPACT_ONLY_G
        if hit:
            lo, hi = max(i - 5, 0), min(i + 11, len(mag))
            peak = lo + int(np.argmax(mag[lo:hi]))
            events.append(peak)
            i = peak + C.WIN          # approximate firmware busy/cooldown time
            continue
        i += 1
    return events


def stream_false_alarms(model, norm, test_subjects):
    files = [f for f in C.OWN_DIR.rglob("*.csv")
             if f.stem.split("_")[0] != "fall"
             and ("own_" + f.parent.name) in test_subjects]
    if not files:
        print("\n(no own non-fall recordings from test subjects: skipping "
              "false-alarm simulation)")
        return
    total_s, alarms, triggers = 0.0, 0, 0
    for f in files:
        sig = load_own_file(f)
        total_s += len(sig) / C.FS
        for p in find_triggers(sig):
            triggers += 1
            w = normalize(window_at(sig, p)[None], norm)
            prob = model.predict(w)[0]
            if prob[C.FALL_IDX] >= C.FALL_PROB_MIN:
                alarms += 1
    hours = total_s / 3600
    print(f"\nStream simulation over {total_s / 60:.1f} min of non-fall data: "
          f"{triggers} triggers, {alarms} CNN fall alarms "
          f"({alarms / max(hours, 1e-9):.2f}/h before the stillness check)")


def main():
    norm = load_json(C.MODELS_DIR / "norm.json")
    splits = load_json(C.MODELS_DIR / "splits.json")
    d = load_windows()
    te = select(d, splits["test"], eval_only=True)
    if not te.any():
        print("No test subjects (too few subjects). Record more people.")
        return
    Xn, y = normalize(d["X"][te], norm), d["y"][te]

    fmodel = tf.keras.models.load_model(C.MODELS_DIR / "final.keras")
    pf = fmodel.predict(Xn, verbose=0)
    rf = report(y, pf.argmax(1), "float32")

    qpath = C.MODELS_DIR / "model_int8.tflite"
    if qpath.exists():
        qmodel = Int8Model(qpath)
        pq = qmodel.predict(Xn)
        rq = report(y, pq.argmax(1), "int8")
        print(f"\nint8 vs float accuracy drop: "
              f"{(rf['accuracy'] - rq['accuracy']) * 100:+.2f} pp; "
              f"argmax agreement {np.mean(pf.argmax(1) == pq.argmax(1)):.3f}")
        stream_false_alarms(qmodel, norm, set(splits["test"]))
    else:
        print("\n(run ml/export.py to evaluate the int8 model)")


if __name__ == "__main__":
    main()
