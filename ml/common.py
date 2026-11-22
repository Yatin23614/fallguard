"""Helpers shared by train / export / evaluate."""
import json

import numpy as np

import config as C


def load_windows():
    d = np.load(C.PROCESSED_DIR / "windows.npz", allow_pickle=False)
    return {k: d[k] for k in d.files}


def split_subjects(subjects, seed=0, frac=(0.7, 0.15, 0.15)):
    """Subject-wise split so no person appears in both train and test.
    SisFall and own subjects are split separately so each split gets both."""
    rng = np.random.default_rng(seed)
    uniq = sorted(set(subjects.tolist()))
    groups = [[s for s in uniq if s.startswith("own_")],
              [s for s in uniq if not s.startswith("own_")]]
    train, val, test = [], [], []
    for g in groups:
        g = list(rng.permutation(g)) if g else []
        n = len(g)
        if n == 0:
            continue
        if n < 3:            # too few people: use them for training only
            train += g
            continue
        n_test = max(1, round(n * frac[2]))
        n_val = max(1, round(n * frac[1]))
        test += g[:n_test]
        val += g[n_test:n_test + n_val]
        train += g[n_test + n_val:]
    return {"train": sorted(train), "val": sorted(val), "test": sorted(test)}


def save_json(obj, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=2))


def load_json(path):
    return json.loads(path.read_text())


def normalize(X, norm):
    """(n, WIN, 6) float -> (n, WIN, 1, 6) normalised model input."""
    mean = np.asarray(norm["mean"], dtype=np.float32)
    std = np.asarray(norm["std"], dtype=np.float32)
    return ((X - mean) / std)[:, :, None, :].astype(np.float32)


def select(d, subjects, eval_only=False):
    """Mask for windows from `subjects`; eval_only keeps unjittered windows."""
    m = np.isin(d["subject"], subjects)
    if eval_only:
        m &= d["offset"] == 0
    return m


def report(y_true, y_pred, title=""):
    k = len(C.CLASSES)
    cm = np.zeros((k, k), dtype=int)
    for t, p in zip(y_true, y_pred):
        cm[t, p] += 1
    print(f"\n== {title} (n={len(y_true)}) ==")
    print("rows=true, cols=pred  " + " ".join(f"{c:>6}" for c in C.CLASSES))
    for i, c in enumerate(C.CLASSES):
        print(f"{c:>21} " + " ".join(f"{v:>6d}" for v in cm[i]))
    is_fall = np.asarray(y_true) == C.FALL_IDX
    pred_fall = np.asarray(y_pred) == C.FALL_IDX
    sens = (pred_fall & is_fall).sum() / max(is_fall.sum(), 1)
    spec = (~pred_fall & ~is_fall).sum() / max((~is_fall).sum(), 1)
    acc = np.trace(cm) / max(cm.sum(), 1)
    print(f"accuracy={acc:.3f}  fall sensitivity={sens:.3f}  "
          f"fall specificity={spec:.3f}")
    for i, c in enumerate(C.CLASSES[1:], start=1):
        m = np.asarray(y_true) == i
        if m.any():
            fa = (pred_fall & m).sum() / m.sum()
            print(f"  {c:>5} misread as fall: {fa:.3f}")
    return {"accuracy": float(acc), "sensitivity": float(sens),
            "specificity": float(spec), "confusion": cm.tolist()}
