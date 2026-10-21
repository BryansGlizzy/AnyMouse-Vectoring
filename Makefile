CC ?= gcc
CFLAGS ?= -O3 -march=native -ffast-math -flto -Wall -Wextra
LDFLAGS ?= -lm

ULX_DEV ?= /dev/input/by-id/usb-Finalmouse_UltralightX_dongle_BE1E28B86F3859BD-event-mouse
SHARK_DEV ?= /dev/input/by-id/usb-_2.4G_Wireless_Mouse-event-mouse

TARGET = octant_daemon

.PHONY: all clean run-ulx run-shark test-boost test-neutral help

all: $(TARGET)

$(TARGET): octant_daemon.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET)

run-ulx: $(TARGET)
	@./start_daemon.sh $(ULX_DEV) 0.25

run-shark: $(TARGET)
	@./start_daemon.sh $(SHARK_DEV) 0.25

test-boost:
	python3 send_weights.py --preset vertical_boost

test-neutral:
	python3 send_weights.py --preset neutral

gui:
	python3 gui.py

help:
	@echo "AnyMouse-Vectoring Build & Run Targets:"
	@echo "  make              - Compile octant_daemon with native optimizations"
	@echo "  make clean        - Remove compiled binary"
	@echo "  make run-ulx      - Run daemon with Finalmouse UltralightX"
	@echo "  make run-shark    - Run daemon with Attack Shark R3"
	@echo "  make gui          - Launch interactive 8-Octant Polar DPI Studio GUI"
	@echo "  make test-boost   - Send vertical boost weights over UDP (port 42069)"
	@echo "  make test-neutral - Reset weights to neutral 0.25"
