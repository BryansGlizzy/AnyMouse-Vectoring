#pragma once
/*
 * Ghost Mode manager for Teensy 4.1:
 * When Bypass is enabled, re-enumerates USB1 with the attached mouse's VID:PID, strings, and
 * up to 2 vendor HID interfaces so official vendor configuration software sees the mouse directly
 * while AnyMouse vectoring stays 1.0x and EP5 (0xFF4D) keeps working.
 */
#include <stdbool.h>

void ghost_set_wanted(bool on);
bool ghost_is_active(void);
void ghost_poll(void);
