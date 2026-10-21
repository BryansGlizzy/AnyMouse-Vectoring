#!/usr/bin/env bash
# start_daemon.sh - Helper script to launch octant_daemon with proper permissions
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$DIR/octant_daemon"

ULX_DEV="/dev/input/by-id/usb-Finalmouse_UltralightX_dongle_BE1E28B86F3859BD-event-mouse"
SHARK_DEV="/dev/input/by-id/usb-_2.4G_Wireless_Mouse-event-mouse"

# Ensure binary is compiled
if [ ! -f "$BIN" ]; then
    echo "[*] Compiling octant_daemon..."
    make -C "$DIR"
fi

# Ensure the uinput kernel module is loaded
if ! lsmod | grep -q "^uinput"; then
    echo "[*] uinput kernel module not currently loaded. Loading..."
    if [ "$(id -u)" -eq 0 ]; then
        if [ -f "$DIR/uinput.ko" ]; then
            insmod "$DIR/uinput.ko" 2>/dev/null || modprobe uinput 2>/dev/null || true
        else
            modprobe uinput 2>/dev/null || true
        fi
    else
        if [ -f "$DIR/uinput.ko" ]; then
            sudo insmod "$DIR/uinput.ko" 2>/dev/null || sudo modprobe uinput 2>/dev/null || true
        else
            sudo modprobe uinput 2>/dev/null || true
        fi
    fi
fi

# Verify /dev/uinput is active
if [ ! -c /dev/uinput ]; then
    echo "[!] /dev/uinput character device does not exist or kernel module failed to load."
    exit 1
fi

# Detect device if not supplied
DEVICE="$1"
SCALE="${2:-0.25}"

if [ -z "$DEVICE" ]; then
    if [ -e "$ULX_DEV" ]; then
        DEVICE="$ULX_DEV"
        echo "[*] Detected Finalmouse UltralightX dongle."
    elif [ -e "$SHARK_DEV" ]; then
        DEVICE="$SHARK_DEV"
        echo "[*] Detected Attack Shark R3 2.4G receiver."
    else
        echo "[!] Neither ULX nor Attack Shark was automatically detected in /dev/input/by-id/."
        echo "Available mouse event devices:"
        ls -l /dev/input/by-id/ | grep -E "mouse" || true
        exit 1
    fi
fi

echo "[*] Launching: sudo $BIN \"$DEVICE\" $SCALE"
echo "[*] Press Ctrl+C at any time to cleanly ungrab the device and exit."

if [ "$(id -u)" -eq 0 ]; then
    exec "$BIN" "$DEVICE" "$SCALE"
else
    exec sudo "$BIN" "$DEVICE" "$SCALE"
fi
