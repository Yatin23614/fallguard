"""Record labelled trials from the ESP32 *logger* firmware over USB serial.

    python ml/record.py --port COM5 --subject alice

For each trial: type a label (fall / sit / lie / other), press Enter to start,
perform the activity, press Enter to stop. Files are saved as
data/own/<subject>/<label>_<timestamp>.csv (t_ms,ax,ay,az,gx,gy,gz).

Tips:
  * fall  : simulated falls onto a thick mattress (forward, backward, sideways,
            from sitting). Stand still ~2 s before, stay down ~3 s after.
  * sit   : sit down soft and hard on different chairs / sofa.
  * lie   : lie down on a bed (from sitting and from standing).
  * other : walking, stairs, bending to pick things up, jumping, long sessions
            of normal activity (these also drive the false-alarm estimate).
"""
import argparse
import threading
import time

import serial

import config as C

HEADER = "t_ms,ax,ay,az,gx,gy,gz"


class Reader(threading.Thread):
    def __init__(self, port, baud):
        super().__init__(daemon=True)
        self.ser = serial.Serial(port, baud, timeout=1)
        self.recording = False
        self.rows = []
        self.lock = threading.Lock()
        self.last_line_t = 0.0

    def run(self):
        while True:
            line = self.ser.readline().decode(errors="ignore").strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(",")
            if len(parts) != 7:
                continue
            try:
                [float(p) for p in parts]
            except ValueError:
                continue
            self.last_line_t = time.time()
            with self.lock:
                if self.recording:
                    self.rows.append(line)

    def start_trial(self):
        with self.lock:
            self.rows, self.recording = [], True

    def stop_trial(self):
        with self.lock:
            self.recording = False
            return list(self.rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="e.g. COM5 or /dev/ttyUSB0")
    ap.add_argument("--subject", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    out_dir = C.OWN_DIR / args.subject
    out_dir.mkdir(parents=True, exist_ok=True)
    r = Reader(args.port, args.baud)
    r.start()
    time.sleep(2)
    if time.time() - r.last_line_t > 2:
        print("WARNING: no data from the logger yet. Is the logger firmware flashed?")

    counts = {}
    while True:
        label = input(f"\nlabel {C.CLASSES} (q to quit): ").strip().lower()
        if label == "q":
            break
        if label not in C.CLASSES:
            print("unknown label")
            continue
        input("Enter to START...")
        r.start_trial()
        t0 = time.time()
        input("recording... Enter to STOP")
        rows = r.stop_trial()
        dur = time.time() - t0
        if len(rows) < C.WIN // 2:
            print(f"only {len(rows)} samples, discarded")
            continue
        name = f"{label}_{time.strftime('%Y%m%d_%H%M%S')}.csv"
        (out_dir / name).write_text(HEADER + "\n" + "\n".join(rows) + "\n")
        counts[label] = counts.get(label, 0) + 1
        print(f"saved {name}: {len(rows)} samples / {dur:.1f} s "
              f"({len(rows) / max(dur, 1e-6):.0f} Hz). Session: {counts}")


if __name__ == "__main__":
    main()
