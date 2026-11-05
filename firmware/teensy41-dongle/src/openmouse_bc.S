/* OpenMouse bundle bytecode (QuickJS-ng 0.17.0), the same file the ESP32-P4 dongle embeds; kept in flash. */
    .section .progmem.openmouse_bc, "a"
    .balign 4
    .global openmouse_bc_start
    .global openmouse_bc_end
openmouse_bc_start:
    .incbin "../esp32p4-dongle/components/amv_js/generated/openmouse.bc"
openmouse_bc_end:
    .byte 0
