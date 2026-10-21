#!/usr/bin/env python3
"""
send_weights.py - Utility to send 8-octant polar sensitivity weights over UDP to octant_daemon.
Listens on UDP 127.0.0.1:42069 by default.
Expected payload: 32 bytes (8 x 32-bit little-endian IEEE-754 floats).

Octants (Cartesian Y-up):
  0: E  (0°)   - Right
  1: NE (45°)  - Up-Right
  2: N  (90°)  - Up
  3: NW (135°) - Up-Left
  4: W  (180°) - Left
  5: SW (225°) - Down-Left
  6: S  (270°) - Down
  7: SE (315°) - Down-Right
"""

import sys
import socket
import struct
import argparse

UDP_IP = "127.0.0.1"
UDP_PORT = 42069

OCTANT_NAMES = ["E (0°)", "NE (45°)", "N (90°)", "NW (135°)", "W (180°)", "SW (225°)", "S (270°)", "SE (315°)"]

PRESETS = {
    "neutral": [0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25],
    "high":    [0.50, 0.50, 0.50, 0.50, 0.50, 0.50, 0.50, 0.50],
    "low":     [0.125, 0.125, 0.125, 0.125, 0.125, 0.125, 0.125, 0.125],
    "vertical_boost": [0.25, 0.35, 0.45, 0.35, 0.25, 0.25, 0.25, 0.25],
    "horizontal_boost": [0.40, 0.25, 0.25, 0.25, 0.40, 0.25, 0.25, 0.25],
    "diagonal_ulx": [0.25, 0.30, 0.25, 0.28, 0.25, 0.22, 0.25, 0.26],
}

def send_weights(weights, host=UDP_IP, port=UDP_PORT):
    if len(weights) != 8:
        raise ValueError(f"Expected 8 weights, got {len(weights)}")
    for i, w in enumerate(weights):
        if w < 0.01 or w > 5.0:
            raise ValueError(f"Weight at index {i} ({w}) out of valid range (0.01 to 5.0)")
    
    payload = struct.pack("8f", *weights)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.sendto(payload, (host, port))
    
    print(f"Sent 8-octant weights to {host}:{port}:")
    for name, w in zip(OCTANT_NAMES, weights):
        print(f"  {name:10s}: {w:.4f}")

def main():
    parser = argparse.ArgumentParser(description="Live-tune 8-octant mouse sensitivity weights via UDP")
    parser.add_argument("weights", nargs="*", type=float, help="1 uniform scalar or 8 individual floats (E, NE, N, NW, W, SW, S, SE)")
    parser.add_argument("--gui", action="store_true", help="Launch interactive graphical tuning interface")
    parser.add_argument("--preset", choices=list(PRESETS.keys()), help="Apply a predefined test profile")
    parser.add_argument("--host", default=UDP_IP, help="Target UDP host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=UDP_PORT, help="Target UDP port (default: 42069)")
    
    args = parser.parse_args()

    if args.gui:
        from gui import launch_gui
        launch_gui()
        return

    if args.preset:
        weights = PRESETS[args.preset]
        print(f"Using preset '{args.preset}':")
    elif len(args.weights) == 1:
        weights = [args.weights[0]] * 8
        print(f"Applying uniform scale {args.weights[0]}:")
    elif len(args.weights) == 8:
        weights = args.weights
    elif len(args.weights) == 0:
        print("No weights specified. Defaulting to neutral (0.25 across all octants).")
        print(f"Available presets: {', '.join(PRESETS.keys())}")
        weights = PRESETS["neutral"]
    else:
        print(f"Error: Expected 1 or 8 weight values, got {len(args.weights)}.", file=sys.stderr)
        sys.exit(1)

    try:
        send_weights(weights, host=args.host, port=args.port)
    except Exception as e:
        print(f"Error sending UDP packet: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()
