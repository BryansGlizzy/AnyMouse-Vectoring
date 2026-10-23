CC ?= gcc
GTK_CFLAGS := $(shell pkg-config --cflags gtk4)
GTK_LIBS   := $(shell pkg-config --libs gtk4)

CFLAGS  ?= -O3 -march=native -ffast-math -flto -Wall -Wextra -pthread $(GTK_CFLAGS)
LDFLAGS ?= -lm -pthread $(GTK_LIBS)

TARGET       = anymouse
FAF_PLUGIN   = libanymouse_faf.so
FAF_LAUNCHER = faf_launcher

SRCS   = src/main.c src/evdev_rt.c src/hid_driver.c src/calibration.c src/gui_gtk4.c \
         src/devices.c src/config.c src/steam_install.c
HDRS   = src/core.h src/kovaaks_offsets.h src/faf_telemetry.h

.PHONY: all install-faf uninstall-faf faf-status install-udev list-devices clean run run-headless probe-hw help

all: $(TARGET) $(FAF_PLUGIN) $(FAF_LAUNCHER) install-faf

$(TARGET): $(SRCS) $(HDRS)
	rm -f $@
	$(CC) $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS)

$(FAF_PLUGIN): src/faf_plugin.c src/faf_telemetry.h
	rm -f $@
	$(CC) -O3 -march=native -shared -fPIC -Wall -Wextra src/faf_plugin.c -o $@ -lm -ldl

$(FAF_LAUNCHER): src/faf_launcher.c
	$(CC) -O2 -Wall -Wextra src/faf_launcher.c -o $@

# Locates FpsAimForge in any Steam library and installs/repairs the launcher, plugin and bundle
install-faf: $(TARGET) $(FAF_PLUGIN) $(FAF_LAUNCHER) faf/AnyMouse.bundle.json
	@./$(TARGET) --install-faf || echo "[!] FpsAimForge integration not installed (see message above)"

uninstall-faf: $(TARGET)
	./$(TARGET) --uninstall-faf

faf-status: $(TARGET)
	@./$(TARGET) --faf-status

install-udev: $(TARGET)
	sudo ./$(TARGET) --install-udev

list-devices: $(TARGET)
	@./$(TARGET) --list-devices

clean:
	rm -f $(TARGET) $(FAF_PLUGIN) $(FAF_LAUNCHER) octant_daemon

run: $(TARGET)
	@./start_daemon.sh

run-headless: $(TARGET)
	@./start_daemon.sh --headless

probe-hw: $(TARGET)
	./$(TARGET) --probe-hw

help:
	@echo "AnyMouse-Vectoring Unified C Build & Run Targets:"
	@echo "  make              - Compile unified './anymouse' binary (RT evdev + HIDraw + KovaaK's Telemetry + GTK4 GUI)"
	@echo "  make run          - Launch './anymouse' with GUI + elevated RT evdev/uinput permissions"
	@echo "  make run-headless - Launch './anymouse --headless' (daemon + auto-calibration CLI mode)"
	@echo "  make list-devices - List detected mice in auto-selection order"
	@echo "  make install-faf  - Find FpsAimForge in your Steam libraries and install/repair the integration"
	@echo "  make uninstall-faf- Restore the original FpsAimForge binary"
	@echo "  make faf-status   - Show FpsAimForge integration status"
	@echo "  make install-udev - Install udev rules so AnyMouse runs without sudo"
	@echo "  make probe-hw     - Query live OpenMouse HID telemetry (CPI, Hz, Battery, Motion Sync, RSSI)"
	@echo "  make clean        - Remove compiled binaries"
