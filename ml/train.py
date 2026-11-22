"""Train the 1D-CNN: pre-train on SisFall, then fine-tune on own MPU-6050 data.

The model is a 1D CNN over time. It is expressed with Conv2D layers using
(k, 1) kernels on a (WIN, 1, 6) input. That gives the same maths as Conv1D,
but it converts to plain CONV_2D / MAX_POOL_2D / AVERAGE_POOL_2D ops, which
every TFLite-Micro build for ESP32 supports.

Usage:
    python ml/train.py [--epochs 40] [--ft-epochs 30]
Outputs: models/pretrained.keras, models/final.keras, models/norm.json,
         models/splits.json
"""
import argparse
import os

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")

import numpy as np
import tensorflow as tf

import config as C
from common import load_windows, normalize, report, save_json, select, split_subjects


def build_model():
    L = tf.keras.layers
    inp = L.Input((C.WIN, 1, C.N_CH), name="imu")
    x = inp
    for filters, k in ((16, 5), (32, 5)):
        x = L.Conv2D(filters, (k, 1), padding="same", use_bias=False)(x)
        x = L.BatchNormalization()(x)
        x = L.ReLU()(x)
        x = L.MaxPooling2D((2, 1))(x)
    x = L.Conv2D(64, (3, 1), padding="same", use_bias=False)(x)
    x = L.BatchNormalization()(x)
    x = L.ReLU()(x)
    x = L.AveragePooling2D((C.WIN // 4, 1))(x)   # global average over time
    x = L.Flatten()(x)
    x = L.Dense(32, activation="relu")(x)
    x = L.Dropout(0.3)(x)
    out = L.Dense(len(C.CLASSES), activation="softmax", name="probs")(x)
    return tf.keras.Model(inp, out)


def class_weights(y):
    counts = np.bincount(y, minlength=len(C.CLASSES)).astype(np.float64)
    counts[counts == 0] = 1
    w = counts.sum() / (len(C.CLASSES) * counts)
    w[C.FALL_IDX] *= 1.5          # missing a fall is worse than a false alarm
    return {i: float(v) for i, v in enumerate(w)}


def fit(model, Xtr, ytr, Xva, yva, epochs, lr, batch=64):
    model.compile(optimizer=tf.keras.optimizers.Adam(lr),
                  loss="sparse_categorical_crossentropy", metrics=["accuracy"])
    cbs = [tf.keras.callbacks.EarlyStopping(
        monitor="val_loss" if len(yva) else "loss", patience=8,
        restore_best_weights=True)]
    model.fit(Xtr, ytr, validation_data=(Xva, yva) if len(yva) else None,
              epochs=epochs, batch_size=batch, class_weight=class_weights(ytr),
              callbacks=cbs, verbose=2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--epochs", type=int, default=40)
    ap.add_argument("--ft-epochs", type=int, default=30)
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()
    tf.keras.utils.set_random_seed(args.seed)

    d = load_windows()
    splits = split_subjects(d["subject"], seed=args.seed)
    print("Subjects:", {k: len(v) for k, v in splits.items()})
    save_json(splits, C.MODELS_DIR / "splits.json")

    tr = select(d, splits["train"])
    va = select(d, splits["val"], eval_only=True)
    te = select(d, splits["test"], eval_only=True)

    # Per-channel normalisation from training data (shipped to the firmware).
    base = d["X"][tr & (d["offset"] == 0)]
    norm = {"mean": base.reshape(-1, C.N_CH).mean(0).tolist(),
            "std": (base.reshape(-1, C.N_CH).std(0) + 1e-6).tolist()}
    save_json(norm, C.MODELS_DIR / "norm.json")

    X = normalize(d["X"], norm)
    y = d["y"]
    sis, own = d["source"] == "sisfall", d["source"] == "own"

    model = build_model()
    model.summary(print_fn=lambda s: print(s) if "params" in s.lower() else None)

    # 1) Pre-train on SisFall (falls back to all data if no SisFall present).
    m_tr = tr & sis if (tr & sis).any() else tr
    m_va = va & sis if (va & sis).any() else va
    print(f"\n--- pre-training on {m_tr.sum()} windows ---")
    fit(model, X[m_tr], y[m_tr], X[m_va], y[m_va], args.epochs, 1e-3)
    model.save(C.MODELS_DIR / "pretrained.keras")

    # 2) Fine-tune on own data, mixed with some SisFall to avoid forgetting.
    if (tr & own).any() and (tr & sis).any():
        own_idx = np.flatnonzero(tr & own)
        sis_idx = np.flatnonzero(tr & sis)
        rng = np.random.default_rng(args.seed)
        n_mix = min(len(sis_idx), 2 * len(own_idx))
        idx = np.concatenate([own_idx, rng.choice(sis_idx, n_mix, replace=False)])
        m_va_ft = va & own if (va & own).any() else va
        print(f"\n--- fine-tuning on {len(own_idx)} own + {n_mix} SisFall windows ---")
        fit(model, X[idx], y[idx], X[m_va_ft], y[m_va_ft], args.ft_epochs, 1e-4)
    else:
        print("\n(no own data and SisFall together: skipping fine-tune)")
    model.save(C.MODELS_DIR / "final.keras")

    if te.any():
        pred = model.predict(X[te], verbose=0).argmax(1)
        report(y[te], pred, "test (float, all sources)")
        for name, m in (("SisFall", sis), ("own", own)):
            if (te & m).any():
                p = model.predict(X[te & m], verbose=0).argmax(1)
                report(y[te & m], p, f"test (float, {name})")
    print(f"\nSaved {C.MODELS_DIR / 'final.keras'}")


if __name__ == "__main__":
    main()
