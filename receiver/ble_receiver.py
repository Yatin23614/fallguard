"""Laptop-side BLE listener for the FallGuard wearable.

    pip install bleak
    python receiver/ble_receiver.py            # ESP32 build (custom GATT service)
    python receiver/ble_receiver.py --hm10     # STM32 build (HM-10 BLE-UART module)
    add --no-ack to leave alerts active instead of auto-acknowledging them

Prints every alert/status message and reconnects automatically.
"""
import argparse
import asyncio
import json
import time

from bleak import BleakClient, BleakScanner

NAME = "FallGuard"
# ESP32: custom service with separate characteristics
SERVICE = "6e3f0001-8b1a-4c2d-9a7e-1f2b3c4d5e6f"
ALERT = "6e3f0002-8b1a-4c2d-9a7e-1f2b3c4d5e6f"
STATUS = "6e3f0003-8b1a-4c2d-9a7e-1f2b3c4d5e6f"
ACK = "6e3f0004-8b1a-4c2d-9a7e-1f2b3c4d5e6f"
# STM32 + HM-10: one UART characteristic, newline-terminated JSON in 20-byte chunks
HM10_SERVICE = "0000ffe0-0000-1000-8000-00805f9b34fb"
HM10_CHAR = "0000ffe1-0000-1000-8000-00805f9b34fb"


def ts():
    return time.strftime("%H:%M:%S")


class Handler:
    """Shared message logic for both boards."""

    def __init__(self, auto_ack: bool):
        self.auto_ack = auto_ack
        self.seen_ids = set()
        self.ack_queue: asyncio.Queue = asyncio.Queue()

    def message(self, text: str):
        try:
            msg = json.loads(text)
        except ValueError:
            return
        if "state" in msg:
            print(f"[{ts()}] status {msg}")
            return
        kind = msg.get("type")
        if kind == "cancel":
            print(f"[{ts()}] alert {msg.get('id')} cancelled by wearer")
        elif kind == "acked":
            print(f"[{ts()}] alert {msg.get('id')} acknowledged")
        elif kind and msg.get("id") not in self.seen_ids:  # skip periodic re-sends
            self.seen_ids.add(msg.get("id"))
            print("\n" + "!" * 60)
            print(f"[{ts()}] {kind.upper()} ALERT: {msg}")
            print("!" * 60 + "\a")
            if self.auto_ack:
                self.ack_queue.put_nowait(msg.get("id"))


async def session_esp32(client, h: Handler):
    await client.start_notify(ALERT, lambda _, d: h.message(d.decode(errors="ignore")))
    await client.start_notify(STATUS, lambda _, d: h.message(d.decode(errors="ignore")))
    current = (await client.read_gatt_char(ALERT)).decode(errors="ignore")
    if current not in ("", "{}"):
        h.message(current)
    while client.is_connected:
        try:
            aid = await asyncio.wait_for(h.ack_queue.get(), 1.0)
            await client.write_gatt_char(ACK, b"ACK", response=True)
            print(f"[{ts()}] acknowledged alert {aid}")
        except asyncio.TimeoutError:
            pass


async def session_hm10(client, h: Handler):
    buf = bytearray()

    def on_data(_, data: bytearray):
        buf.extend(data)
        while b"\n" in buf:
            line, _, rest = bytes(buf).partition(b"\n")
            buf[:] = rest
            h.message(line.decode(errors="ignore").strip())

    await client.start_notify(HM10_CHAR, on_data)
    while client.is_connected:
        try:
            aid = await asyncio.wait_for(h.ack_queue.get(), 1.0)
            await client.write_gatt_char(HM10_CHAR, b"ACK\n", response=False)
            print(f"[{ts()}] acknowledged alert {aid}")
        except asyncio.TimeoutError:
            pass


async def run(auto_ack: bool, hm10: bool):
    h = Handler(auto_ack)
    service = HM10_SERVICE if hm10 else SERVICE
    while True:
        print(f"[{ts()}] scanning for {NAME} ({'HM-10' if hm10 else 'ESP32'}) ...")
        dev = await BleakScanner.find_device_by_filter(
            lambda d, adv: d.name == NAME or service in adv.service_uuids,
            timeout=10)
        if dev is None:
            continue
        try:
            async with BleakClient(dev) as client:
                print(f"[{ts()}] connected to {dev.address}")
                await (session_hm10 if hm10 else session_esp32)(client, h)
        except Exception as e:  # BLE drops are normal; keep trying
            print(f"[{ts()}] connection lost: {e}")
        await asyncio.sleep(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-ack", action="store_true")
    ap.add_argument("--hm10", action="store_true",
                    help="STM32 build: talk to the HM-10 BLE-UART module")
    args = ap.parse_args()
    try:
        asyncio.run(run(auto_ack=not args.no_ack, hm10=args.hm10))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
