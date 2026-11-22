"""Shared constants for the fall-detection ML pipeline.

Anything here that the firmware also needs is exported to
firmware/src/norm_params.h by export.py, so the two sides never drift apart.
"""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# Env overrides let tests run on scratch copies without touching real files.
DATA_DIR = Path(os.environ.get("FALL_DATA_DIR", ROOT / "data"))
SISFALL_DIR = DATA_DIR / "SisFall_dataset"
OWN_DIR = DATA_DIR / "own"
PROCESSED_DIR = DATA_DIR / "processed"
MODELS_DIR = Path(os.environ.get("FALL_MODELS_DIR", ROOT / "models"))
FIRMWARE_SRC = Path(os.environ.get("FALL_FIRMWARE_SRC", ROOT / "firmware" / "src"))
FIRMWARE_STM32_SRC = Path(os.environ.get("FALL_FIRMWARE_STM32_SRC",
                                         ROOT / "firmware_stm32" / "src"))

# ---- Signal / window -------------------------------------------------------
FS = 50                 # Hz, device sample rate
WIN = 128               # samples per window (2.56 s)
PRE = 64                # samples before the impact peak inside the window
N_CH = 6                # ax, ay, az [g], gx, gy, gz [deg/s]
TRAIN_OFFSETS = (-12, -6, 0, 6, 12)   # peak-jitter augmentation (samples)

CLASSES = ["fall", "sit", "lie", "other"]
FALL_IDX = 0

# ---- SisFall ---------------------------------------------------------------
SISFALL_FS = 200
# ADXL345: +-16 g, 13 bit  ->  g per LSB
SISFALL_ACC_SCALE = (2 * 16) / (2 ** 13)
# ITG3200: +-2000 dps, 16 bit -> dps per LSB
SISFALL_GYRO_SCALE = (2 * 2000) / (2 ** 16)

# SisFall activity code -> class
#   D07-D11: sitting down on chairs (incl. collapsing into a chair)
#   D12-D14: lying down / turning while lying
#   F01-F15: falls
#   everything else (walking, jogging, stairs, bending, car, stumble, jump): other
SIT_CODES = {"D07", "D08", "D09", "D10", "D11"}
LIE_CODES = {"D12", "D13", "D14"}


def sisfall_label(code: str) -> str:
    if code.startswith("F"):
        return "fall"
    if code in SIT_CODES:
        return "sit"
    if code in LIE_CODES:
        return "lie"
    return "other"


# Map SisFall sensor axes into the MPU-6050 frame as you mounted it.
# Entry i = (source_axis_index, sign) for device axis i (x, y, z).
# The same mapping is applied to accelerometer and gyroscope.
# Default (identity) assumes the MPU-6050 is clipped to the belt with the same
# orientation as the SisFall device: Y axis vertical. Run
#   python ml/preprocess.py --check-axes
# and compare with the logger output while standing still to verify.
SISFALL_TO_DEVICE = [(0, 1), (1, 1), (2, 1)]

# ---- On-device detector (mirrored in firmware/src/config.h) -----------------
FREEFALL_G = 0.6        # |a| below this counts as free-fall
IMPACT_G = 2.5          # |a| above this after free-fall = impact
IMPACT_ONLY_G = 3.0     # |a| above this triggers even without free-fall
FREEFALL_WINDOW = 50    # samples: free-fall must precede impact by <= 1 s
FALL_PROB_MIN = 0.6     # CNN probability needed to treat event as a fall
