"""Turn SisFall + own recordings into fixed, peak-centred windows.

Output: data/processed/windows.npz with
    X       float32 (n, WIN, 6)   ax, ay, az [g], gx, gy, gz [deg/s] in device frame
    y       int64   (n,)          index into config.CLASSES
    subject str     (n,)          e.g. "SA01" or "own_alice"
    source  str     (n,)          "sisfall" | "own"
    offset  int64   (n,)          peak jitter used for this window (0 = centred)

Usage:
    python ml/preprocess.py                # SisFall + own data
    python ml/preprocess.py --check-axes   # print mean gravity per axis
"""
import argparse
import re
import sys
from pathlib import Path

import numpy as np
from scipy.signal import butter, filtfilt

import config as C

SISFALL_NAME = re.compile(r"^([DF]\d{2})_(S[AE]\d{2})_R(\d{2})\.txt$")


# ---------------------------------------------------------------- loading
def load_sisfall_file(path: Path) -> np.ndarray:
    """Return (n, 6) array at C.FS in device frame (g, deg/s)."""
    rows = []
    with open(path, "r") as f:
        for line in f:
            line = line.strip().rstrip(";").strip()
            if not line:
                continue
            parts = line.split(",")
            if len(parts) < 6:
                continue
            rows.append([int(p) for p in parts[:6]])
    raw = np.asarray(rows, dtype=np.float64)
    acc = raw[:, 0:3] * C.SISFALL_ACC_SCALE
    gyr = raw[:, 3:6] * C.SISFALL_GYRO_SCALE

    # Anti-alias (MPU-6050 DLPF is set to 21 Hz) then 200 -> 50 Hz.
    b, a = butter(4, 20.0 / (C.SISFALL_FS / 2.0))
    sig = np.concatenate([acc, gyr], axis=1)
    if len(sig) > 27:  # filtfilt padlen requirement
        sig = filtfilt(b, a, sig, axis=0)
    step = C.SISFALL_FS // C.FS
    sig = sig[::step]

    out = np.empty_like(sig)
    for dev_axis, (src, sign) in enumerate(C.SISFALL_TO_DEVICE):
        out[:, dev_axis] = sign * sig[:, src]
        out[:, 3 + dev_axis] = sign * sig[:, 3 + src]
    return out.astype(np.float32)


def load_own_file(path: Path) -> np.ndarray:
    data = np.genfromtxt(path, delimiter=",", names=True)
    cols = ["ax", "ay", "az", "gx", "gy", "gz"]
    return np.stack([data[c] for c in cols], axis=1).astype(np.float32)


# ---------------------------------------------------------------- windowing
def window_at(sig: np.ndarray, peak: int) -> np.ndarray:
    """WIN samples with `peak` at index PRE; edge-padded if needed."""
    start = peak - C.PRE
    lo, hi = max(start, 0), min(start + C.WIN, len(sig))
    w = sig[lo:hi]
    pad_before = lo - start
    pad_after = C.WIN - pad_before - len(w)
    if pad_before or pad_after:
        w = np.pad(w, ((pad_before, pad_after), (0, 0)), mode="edge")
    return w


def trial_windows(sig: np.ndarray, label: str, rng: np.random.Generator):
    """Yield (window, offset) pairs for one trial."""
    mag = np.linalg.norm(sig[:, :3], axis=1)
    peak = int(np.argmax(mag))
    for off in C.TRAIN_OFFSETS:
        yield window_at(sig, peak + off), off
    if label == "other" and len(sig) > 2 * C.WIN:
        # Daily activity: add a few random windows so the CNN also sees
        # non-peak segments (walking, stairs...). Marked offset=0 => usable
        # for evaluation too.
        for _ in range(3):
            p = int(rng.integers(C.PRE, len(sig) - (C.WIN - C.PRE)))
            yield window_at(sig, p), 0


# ---------------------------------------------------------------- datasets
def collect_sisfall(rng):
    X, y, subj, src, offs = [], [], [], [], []
    files = sorted(C.SISFALL_DIR.rglob("*.txt"))
    n_used = 0
    for f in files:
        m = SISFALL_NAME.match(f.name)
        if not m:
            continue
        code, subject, _ = m.groups()
        label = C.sisfall_label(code)
        sig = load_sisfall_file(f)
        if len(sig) < C.WIN // 2:
            continue
        n_used += 1
        for w, off in trial_windows(sig, label, rng):
            X.append(w); y.append(C.CLASSES.index(label))
            subj.append(subject); src.append("sisfall"); offs.append(off)
    print(f"SisFall: {n_used} trials -> {len(X)} windows")
    return X, y, subj, src, offs


def collect_own(rng):
    """data/own/<subject>/<label>_<anything>.csv"""
    X, y, subj, src, offs = [], [], [], [], []
    n_used = 0
    for f in sorted(C.OWN_DIR.rglob("*.csv")):
        label = f.stem.split("_")[0]
        if label not in C.CLASSES:
            print(f"  skip {f} (unknown label '{label}')")
            continue
        sig = load_own_file(f)
        if len(sig) < C.WIN // 2:
            continue
        n_used += 1
        subject = "own_" + f.parent.name
        for w, off in trial_windows(sig, label, rng):
            X.append(w); y.append(C.CLASSES.index(label))
            subj.append(subject); src.append("own"); offs.append(off)
    print(f"Own data: {n_used} trials -> {len(X)} windows")
    return X, y, subj, src, offs


def check_axes():
    """Mean accel of the first second of SisFall walking trials (person upright)."""
    means = []
    for f in sorted(C.SISFALL_DIR.rglob("D01_*.txt"))[:20]:
        sig = load_sisfall_file(f)
        means.append(sig[: C.FS, :3].mean(axis=0))
    if not means:
        sys.exit(f"No SisFall D01 files under {C.SISFALL_DIR}")
    m = np.mean(means, axis=0)
    print("SisFall (mapped to device frame) mean accel while upright [g]:")
    print(f"  ax={m[0]:+.2f}  ay={m[1]:+.2f}  az={m[2]:+.2f}")
    print("Compare with the logger firmware while you stand still wearing the")
    print("device. Edit SISFALL_TO_DEVICE in ml/config.py until they agree.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check-axes", action="store_true")
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    if args.check_axes:
        check_axes()
        return

    rng = np.random.default_rng(args.seed)
    parts = [collect_sisfall(rng), collect_own(rng)]
    X = sum((p[0] for p in parts), [])
    if not X:
        sys.exit("No data found. See README: download SisFall and/or record own data.")
    y = sum((p[1] for p in parts), [])
    subj = sum((p[2] for p in parts), [])
    src = sum((p[3] for p in parts), [])
    offs = sum((p[4] for p in parts), [])

    C.PROCESSED_DIR.mkdir(parents=True, exist_ok=True)
    out = C.PROCESSED_DIR / "windows.npz"
    np.savez_compressed(
        out,
        X=np.stack(X).astype(np.float32),
        y=np.asarray(y, dtype=np.int64),
        subject=np.asarray(subj),
        source=np.asarray(src),
        offset=np.asarray(offs, dtype=np.int64),
    )
    counts = np.bincount(np.asarray(y), minlength=len(C.CLASSES))
    print("Class counts:", dict(zip(C.CLASSES, counts.tolist())))
    print(f"Saved {out}")


if __name__ == "__main__":
    main()
