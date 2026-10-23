#!/usr/bin/env bash
# start_daemon.sh - Helper script to launch the unified './anymouse' C binary
# Preserves Wayland/X11 display environment variables when elevating via sudo so the GTK4 GUI works seamlessly.
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$DIR/anymouse"

# The mouse is auto-detected (or chosen in the GUI). Pass a /dev/input path to force one.
# Set ANYMOUSE_NO_SUDO=1 after `make install-udev` to run without sudo (no SCHED_FIFO).

# Ensure binary is compiled
if [ ! -f "$BIN" ]; then
    echo "[*] Compiling unified anymouse binary..."
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

echo "[*] Launching unified AnyMouse engine: $BIN $*"
echo "[*] Press Ctrl+C or close the window at any time to cleanly ungrab the mouse and exit."

if [ "$(id -u)" -eq 0 ] || [ "${ANYMOUSE_NO_SUDO:-0}" = "1" ]; then
    exec "$BIN" "$@"
else
    exec sudo -E env \
        DISPLAY="${DISPLAY:-}" \
        WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-}" \
        XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-}" \
        XAUTHORITY="${XAUTHORITY:-}" \
        "$BIN" "$@"
fi
