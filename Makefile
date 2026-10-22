CC ?= gcc
GTK_CFLAGS := $(shell pkg-config --cflags gtk4)
GTK_LIBS   := $(shell pkg-config --libs gtk4)

CFLAGS  ?= -O3 -march=native -ffast-math -flto -Wall -Wextra -pthread $(GTK_CFLAGS)
LDFLAGS ?= -lm -pthread $(GTK_LIBS)

ULX_DEV   ?= /dev/input/by-id/usb-Finalmouse_UltralightX_dongle_BE1E28B86F3859BD-event-mouse
SHARK_DEV ?= /dev/input/by-id/usb-_2.4G_Wireless_Mouse-event-mouse

TARGET       = anymouse
FAF_PLUGIN   = libanymouse_faf.so
FAF_LAUNCHER = faf_launcher
FAF_DIR      = /mnt/iscsi-nvme/SteamLibrary/steamapps/common/FpsAimForge
FAF_USER_DIR = $(HOME)/.local/share/FpsAimForge/bundles

SRCS   = src/main.c src/evdev_rt.c src/hid_driver.c src/calibration.c src/gui_gtk4.c
HDRS   = src/core.h src/kovaaks_offsets.h src/faf_telemetry.h

.PHONY: all install-faf clean run run-headless run-ulx run-shark probe-hw help

all: $(TARGET) $(FAF_PLUGIN) $(FAF_LAUNCHER) install-faf

$(TARGET): $(SRCS) $(HDRS)
	rm -f $@
	$(CC) $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS)

$(FAF_PLUGIN): src/faf_plugin.c src/faf_telemetry.h
	rm -f $@
	$(CC) -O3 -march=native -shared -fPIC -Wall -Wextra src/faf_plugin.c -o $@ -lm -ldl

$(FAF_LAUNCHER): src/faf_launcher.c
	$(CC) -O2 -Wall -Wextra src/faf_launcher.c -o $@

install-faf: $(FAF_PLUGIN) $(FAF_LAUNCHER) faf/AnyMouse.bundle.json
	@mkdir -p "$(FAF_USER_DIR)"
	@cp -f faf/AnyMouse.bundle.json "$(FAF_USER_DIR)/AnyMouse.bundle.json"
	@if [ -d "$(FAF_DIR)" ]; then \
		if [ -f "$(FAF_DIR)/FpsAimForge" ] && [ ! -f "$(FAF_DIR)/FpsAimForge.bin" ]; then \
			cp -p "$(FAF_DIR)/FpsAimForge" "$(FAF_DIR)/FpsAimForge.bin"; \
		fi; \
		if [ -f "$(FAF_DIR)/FpsAimForge.bin" ]; then \
			cp -f $(FAF_LAUNCHER) "$(FAF_DIR)/FpsAimForge"; \
			chmod +x "$(FAF_DIR)/FpsAimForge" "$(FAF_DIR)/FpsAimForge.bin"; \
		fi; \
	fi

clean:
	rm -f $(TARGET) $(FAF_PLUGIN) $(FAF_LAUNCHER) octant_daemon

run: $(TARGET)
	@./start_daemon.sh

run-headless: $(TARGET)
	@./start_daemon.sh --headless

run-ulx: $(TARGET)
	@./start_daemon.sh $(ULX_DEV) 0.25

run-shark: $(TARGET)
	@./start_daemon.sh $(SHARK_DEV) 0.25

probe-hw: $(TARGET)
	./$(TARGET) --probe-hw

help:
	@echo "AnyMouse-Vectoring Unified C Build & Run Targets:"
	@echo "  make              - Compile unified './anymouse' binary (RT evdev + HIDraw + KovaaK's Telemetry + GTK4 GUI)"
	@echo "  make run          - Launch './anymouse' with GUI + elevated RT evdev/uinput permissions"
	@echo "  make run-headless - Launch './anymouse --headless' (daemon + auto-calibration CLI mode)"
	@echo "  make run-ulx      - Run with Finalmouse UltralightX @ 0.25x scale"
	@echo "  make run-shark    - Run with Attack Shark R3 @ 0.25x scale"
	@echo "  make probe-hw     - Query live OpenMouse HID telemetry (CPI, Hz, Battery, Motion Sync, RSSI)"
	@echo "  make clean        - Remove compiled binaries"
