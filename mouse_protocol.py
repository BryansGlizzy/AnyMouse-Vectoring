#!/usr/bin/env python3
"""
mouse_protocol.py - Native Hardware Telemetry & Configuration Bridge
Derived from the OpenMouse Project (https://github.com/OpenMouse-Project/mouse-protocol).

Provides direct WebHID/hidraw access on Linux to query and configure gaming mice:
  - Finalmouse UltralightX (VID 0x361D, PID 0x0100)
  - Attack Shark R3 / R2 & GearHub OEM (VID 0x3151, PID 0x402D)

Capabilities:
  - Auto-detects connected mouse model & active /dev/hidrawX device node
  - Reads active hardware CPI setting, polling rate (Hz), battery % & voltage,
    Motion Sync state, LOD, RSSI signal strength, and firmware versions
  - Sets physical sensor CPI, polling rate, and Motion Sync directly via HID reports
"""

import os
import glob
import time
import select
import struct
from typing import Dict, Any, Optional, Tuple

# --- Finalmouse Constants ---
FINALMOUSE_VID = 0x361D
FINALMOUSE_ULX_PID = 0x0100
FINALMOUSE_REPORT_MAIN = 0x04
FINALMOUSE_REPORT_INPUT = 0x05
FINALMOUSE_REPORT_DONGLE = 0x02

FINALMOUSE_CMD = {
    "dpi": 16,
    "pollingRate": 17,
    "motionSync": 18,
    "dongleLed": 20,
    "liftOffDistance": 21,
    "tournamentScrollMode": 23,
    "tournamentScrollTimeout": 24,
    "wakeAll": 96,
    "dongleInfo": 10,
}

# --- GearHub / Attack Shark Constants ---
GEARHUB_VID = 0x3151
GEARHUB_PID_24G = 0x402D
GEARHUB_REPORT_SIZE = 64


def find_supported_devices() -> list[Dict[str, Any]]:
    """Scan /sys/class/hidraw to identify connected supported gaming mice."""
    devices = []
    for uevent_path in sorted(glob.glob("/sys/class/hidraw/hidraw*/device/uevent")):
        try:
            with open(uevent_path, "r") as f:
                content = f.read()
            
            lines = dict(line.split("=", 1) for line in content.splitlines() if "=" in line)
            hid_id = lines.get("HID_ID", "")
            hid_name = lines.get("HID_NAME", "")
            dev_node = "/dev/" + os.path.basename(os.path.dirname(os.path.dirname(uevent_path)))

            # Check permissions
            accessible = os.access(dev_node, os.R_OK | os.W_OK)

            # HID_ID format: bus:vendor:product (hex)
            parts = hid_id.split(":")
            if len(parts) >= 3:
                vid = int(parts[1], 16)
                pid = int(parts[2], 16)

                if vid == FINALMOUSE_VID and pid == FINALMOUSE_ULX_PID:
                    devices.append({
                        "brand": "Finalmouse",
                        "model": "UltralightX",
                        "vid": vid,
                        "pid": pid,
                        "node": dev_node,
                        "name": hid_name,
                        "accessible": accessible,
                        "type": "finalmouse"
                    })
                elif vid == GEARHUB_VID and pid == GEARHUB_PID_24G:
                    # Input0 is movement, input1/input2 are config endpoints
                    phys = lines.get("HID_PHYS", "")
                    devices.append({
                        "brand": "Attack Shark / GearHub",
                        "model": "R3 / 2.4G Wireless",
                        "vid": vid,
                        "pid": pid,
                        "node": dev_node,
                        "name": hid_name,
                        "phys": phys,
                        "accessible": accessible,
                        "type": "gearhub"
                    })
        except Exception:
            continue
    return devices


class FinalmouseDriver:
    """Hardware driver for Finalmouse UltralightX over Linux hidraw."""

    @staticmethod
    def _build_report(command: int, payload: bytes = b"") -> bytes:
        out = bytearray(63)
        out[0] = 2 + len(payload)
        out[1] = 0x80 | (command & 0x7F)
        out[2] = len(payload)
        out[3:3 + len(payload)] = payload
        return bytes([FINALMOUSE_REPORT_MAIN]) + bytes(out)

    @staticmethod
    def read_status(dev_node: str, timeout_sec: float = 0.5) -> Dict[str, Any]:
        """Query Finalmouse ULX and parse the complete telemetry burst."""
        fd = os.open(dev_node, os.O_RDWR | os.O_NONBLOCK)
        try:
            # Send wakeAll command to trigger telemetry burst
            req = FinalmouseDriver._build_report(FINALMOUSE_CMD["wakeAll"])
            os.write(fd, req)

            # Collect response reports
            responses = []
            deadline = time.time() + timeout_sec
            while time.time() < deadline:
                r, _, _ = select.select([fd], [], [], 0.04)
                if r:
                    try:
                        chunk = os.read(fd, 64)
                        if chunk:
                            responses.append(chunk)
                    except OSError:
                        break

            return FinalmouseDriver._decode_telemetry(responses)
        finally:
            os.close(fd)

    @staticmethod
    def _decode_telemetry(reports: list[bytes]) -> Dict[str, Any]:
        status: Dict[str, Any] = {
            "brand": "Finalmouse",
            "model": "UltralightX",
            "connected": len(reports) > 0,
        }

        for data in reports:
            if len(data) < 4 or data[0] != FINALMOUSE_REPORT_INPUT:
                continue

            inner_len = min(data[1], len(data) - 2)
            body = data[2:2 + inner_len]
            if len(body) < 2:
                continue

            cmd = body[0]
            p_len = min(body[1], len(body) - 2)
            payload = body[2:2 + p_len]

            if cmd in (3, 16):  # DPI
                status["dpi"] = struct.unpack("<H", payload[:2])[0]
            elif cmd in (4, 17):  # Polling rate
                status["polling_rate_hz"] = struct.unpack("<H", payload[:2])[0]
            elif cmd == 5:  # Battery millivolts
                mv = struct.unpack("<H", payload[:2])[0]
                status["battery_mv"] = mv
                # 3.4V to 4.2V standard LiPo curve
                pct = max(0, min(100, int((mv - 3400) / (4200 - 3400) * 100)))
                status["battery_pct"] = pct
            elif cmd == 11:
                status["dongle_rf_fw"] = payload.rstrip(b"\x00").decode("ascii", errors="ignore")
            elif cmd == 12:
                status["mouse_fw"] = payload.rstrip(b"\x00").decode("ascii", errors="ignore")
            elif cmd == 13:  # RSSI in dBm
                rssi = payload[0] if payload[0] <= 127 else payload[0] - 256
                status["rssi_dbm"] = rssi
            elif cmd == 14:
                status["serial"] = payload.rstrip(b"\x00").decode("ascii", errors="ignore")
            elif cmd == 18:  # Motion Sync
                status["motion_sync"] = (payload[0] == 1)
            elif cmd == 21:  # Lift-off distance
                status["lod_mm"] = payload[0]

        return status

    @staticmethod
    def set_dpi(dev_node: str, dpi: int) -> bool:
        """Set hardware physical sensor DPI on Finalmouse ULX (50-26000)."""
        if not (50 <= dpi <= 26000):
            raise ValueError(f"DPI {dpi} out of range (50 - 26,000)")
        payload = struct.pack("<H", dpi)
        req = FinalmouseDriver._build_report(FINALMOUSE_CMD["dpi"], payload)
        fd = os.open(dev_node, os.O_RDWR)
        try:
            os.write(fd, req)
            return True
        finally:
            os.close(fd)

    @staticmethod
    def set_motion_sync(dev_node: str, enabled: bool) -> bool:
        """Toggle hardware Motion Sync on Finalmouse ULX."""
        payload = bytes([1 if enabled else 0])
        req = FinalmouseDriver._build_report(FINALMOUSE_CMD["motionSync"], payload)
        fd = os.open(dev_node, os.O_RDWR)
        try:
            os.write(fd, req)
            return True
        finally:
            os.close(fd)

    @staticmethod
    def set_polling_rate(dev_node: str, hz: int) -> bool:
        """Set hardware polling rate on Finalmouse ULX (500, 1000, 2000, 4000, 8000)."""
        if hz not in (500, 1000, 2000, 4000, 8000):
            raise ValueError(f"Unsupported polling rate {hz} Hz")
        payload = struct.pack("<H", hz)
        req = FinalmouseDriver._build_report(FINALMOUSE_CMD["pollingRate"], payload)
        fd = os.open(dev_node, os.O_RDWR)
        try:
            os.write(fd, req)
            return True
        finally:
            os.close(fd)


class MouseHardwareManager:
    """Unified interface to discover and interact with supported gaming mice."""

    @classmethod
    def get_active_mouse_status(cls) -> Optional[Dict[str, Any]]:
        devices = find_supported_devices()
        if not devices:
            return None

        # Prefer Finalmouse ULX if connected
        ulx_dev = next((d for d in devices if d["type"] == "finalmouse"), None)
        if ulx_dev:
            try:
                status = FinalmouseDriver.read_status(ulx_dev["node"])
                status["node"] = ulx_dev["node"]
                return status
            except Exception as e:
                return {"brand": "Finalmouse", "model": "UltralightX", "error": str(e), "node": ulx_dev["node"]}

        return None

    @classmethod
    def set_mouse_cpi(cls, cpi: int) -> bool:
        devices = find_supported_devices()
        ulx_dev = next((d for d in devices if d["type"] == "finalmouse"), None)
        if ulx_dev:
            return FinalmouseDriver.set_dpi(ulx_dev["node"], cpi)
        return False

    @classmethod
    def set_mouse_motion_sync(cls, enabled: bool) -> bool:
        devices = find_supported_devices()
        ulx_dev = next((d for d in devices if d["type"] == "finalmouse"), None)
        if ulx_dev:
            return FinalmouseDriver.set_motion_sync(ulx_dev["node"], enabled)
        return False


def main():
    print("=== OpenMouse Hardware Telemetry Probe ===")
    devices = find_supported_devices()
    print(f"Found {len(devices)} supported mouse HID nodes:")
    for d in devices:
        acc = "OK (rw)" if d["accessible"] else "NO PERMISSIONS (check udev)"
        print(f"  [{d['brand']} - {d['model']}] {d['node']} (VID:{d['vid']:#06x} PID:{d['pid']:#06x}) - {acc}")

    print("\nQuerying active mouse telemetry...")
    status = MouseHardwareManager.get_active_mouse_status()
    if status:
        print("\n--- Live Hardware Telemetry ---")
        for k, v in status.items():
            print(f"  {k:20s}: {v}")
    else:
        print("No active supported mouse responded to telemetry queries.")


if __name__ == "__main__":
    main()
