# AnyMouse Studio (web)

The no-install front-end for the AnyMouse dongle. It tunes the eight direction weights, manages the four profile slots stored on the dongle, and shows the dongle's state, all over the dongle's USB vendor-HID channel using WebHID. Static files, no build step, no server component, nothing leaves your computer.

**Verified** in Chrome against the real board (2026-10-26): connect, live state, weight changes taking effect on the cursor. **Mouse card:** shows every device plugged into the dongle: its tier (known protocol, generic HID, boot fallback, not used), name, VID:PID, polling rate (what the mouse reports, its USB interval, and a live measured rate while you move it), CPI (read from the mouse, or entered by you for mice without a protocol), battery and firmware where the mouse reports them, plus the report layout. CPI and polling controls appear only for mouse models verified through the dongle; **Download descriptor** saves the mouse's raw HID report descriptor as a `.hex` file.

**Needs** Chrome, Edge, Brave or Opera on a computer. Firefox, Safari and phones have no WebHID; they use the native app (planned). The page says so if the browser cannot do it.

## Run it
```bash
python3 -m http.server 8765 --directory web      # from the repo root
# then open http://localhost:8765 in Chrome, plug in the dongle's PC-facing USB-C, press "Connect dongle"
```
`localhost` counts as a secure context, which WebHID requires. For a shared deployment, host the `web/` folder on any HTTPS static host (GitHub Pages works). **Demo mode** (`/?demo`) runs the whole UI against a simulated dongle built into the page, so you can try it without hardware.

## Use it
- Drag a handle on the chart, or use the sliders and number boxes. Changes go to the dongle at up to about 30 per second, and it saves the active slot by itself two seconds after the last change.
- **Presets**, **Master** (scales every direction together, keeping the shape) and the **Link** boxes (editing a direction also sets its left/right or up/down mirror).
- **Profiles:** pick a slot, then *Load* or *Save current to slot*. A green dot means the slot holds a saved profile.
- **Bypass** sets every direction to 1.0 without touching your profile. It is not saved, so it resets on power-up.
- **Export / Import JSON** writes this app's format and also reads a `profile.json` from the Linux engine (the shape is kept and scaled so the average is 1.0, because the Linux weights also divide down a high sensor CPI).
- If something else changes the dongle (another tab, the native app, the dongle itself) the page updates; if you edit against an outdated revision your edit is dropped and the page refreshes, with a message.
- **Developer tools** run a movement through the dongle's real scaling code (it does not move your cursor) and read its counters.
- *Mouse CPI* only labels the effective DPI per direction; the dongle does not need it.

## Linux
The browser needs permission to open the dongle's `/dev/hidraw*` node:
```bash
echo 'SUBSYSTEM=="hidraw", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="4d56", TAG+="uaccess"' | sudo tee /etc/udev/rules.d/71-anymouse-dongle.rules
sudo udevadm control --reload-rules && sudo udevadm trigger --action=add --subsystem-match=hidraw
```
`./anymouse --install-udev` installs an equivalent rule keyed on the `AnyMouse` manufacturer string, which keeps working if the dongle's USB ID changes.

**Identity:** the page matches only the dongle's vendor usage page (`0xFF4D`) and then proves the device with the `HELLO` handshake (random nonce echoed back with the `AMVD` identity word), exactly like the native app, so a changed USB VID:PID does not matter. Firmware older than `HELLO` (0.3.x) is accepted through `GET_INFO`.

Flatpak and Snap browsers cannot see hidraw devices; use a native package. On Windows, only one program can hold the dongle's HID interface at a time, so close the native app first.

## Files
| File | Purpose |
|:---|:---|
| `index.html`, `style.css`, `app.js` | The page and its logic (state, edits, connection handling) |
| `radar.js` | The polar chart. Its outline is the real cosine-blended sensitivity envelope, not straight lines between handles |
| `dongle.js` | Protocol client (frame encode/decode, requests, events). Works on any WebHID-shaped object |
| `proto.js` | **Generated** from [`common/amv_dongle_proto.h`](../common/amv_dongle_proto.h); do not edit. Regenerate with `python3 -I firmware/esp32p4-dongle/tools/gen_proto_js.py` |
| `presets.js` | Direction names, presets, symmetry pairs, profile import/export |
| `mock_device.js` | A JavaScript twin of the firmware's control plane: powers `?demo` and the tests |
| `sw.js`, `manifest.webmanifest`, `icon.svg` | Offline shell (network first) and install metadata |
| `tests/` | Node tests and `fixtures/frames.json`, frames recorded from the real board |

## Tests
```bash
cd web && node --test tests/*.test.mjs        # 16 tests, no dependencies
```
They check that `proto.js` is current, that the client's requests are byte-identical to the ones recorded from the real firmware, that the demo device answers like the real firmware (replayed from the recording), and the client's behaviour (stale and invalid writes, events, timeouts, profile import). To re-record against a connected dongle: `python3 -I firmware/esp32p4-dongle/tools/capture_fixtures.py` (needs `pip install hidapi`).

**Not covered by automated tests:** the browser's WebHID permission prompt and a real drag on the chart. Those need a person in Chrome.

Protocol, firmware and milestones: [firmware/esp32p4-dongle/PLAN.md](../firmware/esp32p4-dongle/PLAN.md).
