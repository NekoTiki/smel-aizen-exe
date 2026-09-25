#pragma once
// The little the shared core needs from the hardware. Implemented by
// src/platform/esp32/main.cpp (real pins) and src/platform/native/main.cpp (PC twin).

#include <stddef.h>
#include <stdint.h>

namespace hal {

uint32_t millis();
void pinOutput(uint8_t pin);
void pinWrite(uint8_t pin, bool high);
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace hal
