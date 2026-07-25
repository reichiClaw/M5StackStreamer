#pragma once

#include <Arduino.h>

namespace diagnostics {

// Writes a timestamped line to Serial and a fixed-size in-memory ring buffer.
// Wi-Fi credentials and stream audio are never recorded.
void logf(const char* format, ...)
    __attribute__((format(printf, 1, 2)));

// Returns the current ring buffer in chronological order.
String snapshot();

}  // namespace diagnostics
