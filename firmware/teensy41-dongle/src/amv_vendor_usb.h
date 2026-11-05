#pragma once

#include <stdint.h>

bool amv_vendor_recv(uint8_t frame[64]);
bool amv_vendor_send(const uint8_t frame[64]);
