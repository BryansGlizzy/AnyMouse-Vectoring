#include "amv_vendor_usb.h"

#include <usb_rawhid.h>

bool amv_vendor_recv(uint8_t frame[64]) {
    return usb_rawhid_recv(frame, 0) == 64;
}

bool amv_vendor_send(const uint8_t frame[64]) {
    return usb_rawhid_send(frame, 0) == 64;
}
